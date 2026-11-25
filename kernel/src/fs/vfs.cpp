// SPDX-License-Identifier: GPL-2.0-only
#include "fs/vfs.h"
#include "arch/x86_64/apic.h"
#include "lib/string.h"
#include "mm/heap.h"

namespace fs {
namespace {

Node* g_root = nullptr;
uint64_t g_used = 0, g_files = 0;

char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; }

int name_cmp(const char* a, const char* b) {
    for (;; a++, b++) {
        char x = lower(*a), y = lower(*b);
        if (x != y || !x) return int(uint8_t(x)) - int(uint8_t(y));
    }
}

// Ordering: directories first, then case-insensitive name.
bool before(const Node* a, const Node* b) {
    if (a->dir != b->dir) return a->dir;
    return name_cmp(a->name, b->name) < 0;
}

void link(Node* dir, Node* n) {
    n->parent = dir;
    Node** pp = &dir->child;
    while (*pp && before(*pp, n)) pp = &(*pp)->next;
    n->next = *pp;
    *pp = n;
}

void unlink(Node* n) {
    if (!n->parent) return;
    Node** pp = &n->parent->child;
    while (*pp && *pp != n) pp = &(*pp)->next;
    if (*pp) *pp = n->next;
    n->parent = nullptr;
    n->next = nullptr;
}

bool valid_name(const char* name) {
    size_t n = strlen(name);
    if (n == 0 || n > NAME_MAX) return false;
    if ((n == 1 && name[0] == '.') || (n == 2 && name[0] == '.' && name[1] == '.')) return false;
    for (size_t i = 0; i < n; i++)
        if (name[i] == '/' || uint8_t(name[i]) < 0x20) return false;
    return true;
}

Node* new_node(const char* name, bool dir) {
    auto* n = static_cast<Node*>(kzalloc(sizeof(Node)));
    if (!n) return nullptr;
    size_t l = strlen(name);
    if (l > NAME_MAX) l = NAME_MAX;
    memcpy(n->name, name, l);
    n->name[l] = 0;
    n->dir = dir;
    n->mtime_ms = apic::uptime_ms();
    return n;
}

Node* make(Node* dir, const char* name, bool is_dir, Error* err) {
    auto fail = [&](Error e) -> Node* {
        if (err) *err = e;
        return nullptr;
    };
    if (!dir || !dir->dir) return fail(NotDir);
    if (!valid_name(name)) return fail(BadName);
    if (find_child(dir, name)) return fail(Exists);
    Node* n = new_node(name, is_dir);
    if (!n) return fail(NoMemory);
    link(dir, n);
    if (dir->ops) {
        n->ops = dir->ops;
        n->mnt = dir->mnt;
        n->loaded = true;  // new and empty
        Error e = dir->ops->create(dir, n);
        if (e != Ok) {
            unlink(n);
            kfree(n);
            return fail(e);
        }
    }
    if (!is_dir) g_files++;
    if (err) *err = Ok;
    return n;
}

void free_tree(Node* n) {
    while (n->child) {
        Node* c = n->child;
        n->child = c->next;
        free_tree(c);
    }
    if (!n->dir) {
        if (n->loaded || !n->ops) g_used -= n->size;
        g_files--;
    }
    kfree(n->data);
    kfree(n);
}

}  // namespace

const char* error_str(Error e) {
    switch (e) {
    case Ok: return "OK";
    case NotFound: return "не найдено";
    case Exists: return "уже существует";
    case NotDir: return "не папка";
    case IsDir: return "это папка";
    case NoMemory: return "не хватает памяти";
    case BadName: return "недопустимое имя";
    case Recursive: return "нельзя копировать папку в саму себя";
    case Busy: return "занято";
    case ReadOnly: return "только для чтения";
    case IoError: return "ошибка ввода-вывода";
    case NoSpace: return "нет места на диске";
    }
    return "?";
}

void init() {
    if (!g_root) g_root = new_node("", true);
}

Node* root() { return g_root; }

bool ensure_dir(Node* dir) {
    if (!dir || !dir->dir) return false;
    if (dir->ops && !dir->loaded) {
        dir->loaded = true;  // set first: load_dir attaches children
        if (!dir->ops->load_dir(dir)) {
            dir->loaded = false;
            return false;
        }
    }
    return true;
}

bool ensure_data(Node* f) {
    if (!f || f->dir) return false;
    if (f->ops && !f->loaded) {
        if (!f->ops->load_file(f)) return false;
        f->loaded = true;
    }
    return true;
}

Node* attach(Node* dir, const char* name, bool is_dir) {
    Node* n = new_node(name, is_dir);
    if (!n) return nullptr;
    n->ops = dir->ops;
    n->mnt = dir->mnt;
    link(dir, n);
    if (!is_dir) g_files++;
    return n;
}

bool set_data(Node* f, const void* data, size_t len) {
    if (len > f->cap) {
        void* p = kmalloc(len);
        if (!p) return false;
        kfree(f->data);
        f->data = static_cast<uint8_t*>(p);
        f->cap = len;
    }
    if (len) memcpy(f->data, data, len);
    if (f->loaded || !f->ops) g_used -= f->size;
    f->size = len;
    g_used += len;
    f->loaded = true;
    return true;
}

Node* mount(const char* name, const Ops* ops, void* mnt, uint64_t a, uint64_t b, uint64_t c) {
    Node* existing = find_child(g_root, name);
    if (existing) remove(existing);
    Node* n = new_node(name, true);
    if (!n) return nullptr;
    n->ops = ops;
    n->mnt = mnt;
    n->fs_a = a;
    n->fs_b = b;
    n->fs_c = c;
    link(g_root, n);
    return n;
}

Node* find_child(Node* dir, const char* name) {
    if (!dir || !dir->dir) return nullptr;
    ensure_dir(dir);
    for (Node* c = dir->child; c; c = c->next)
        if (strlen(c->name) == strlen(name) && memcmp(c->name, name, strlen(name)) == 0) return c;
    return nullptr;
}

Node* lookup(const char* path) {
    Node* n = g_root;
    char part[NAME_MAX + 1];
    while (n && *path) {
        while (*path == '/') path++;
        if (!*path) break;
        size_t l = 0;
        while (path[l] && path[l] != '/') l++;
        if (l > NAME_MAX) return nullptr;
        memcpy(part, path, l);
        part[l] = 0;
        path += l;
        if (l == 1 && part[0] == '.') continue;
        if (l == 2 && part[0] == '.' && part[1] == '.') {
            if (n->parent) n = n->parent;
            continue;
        }
        n = find_child(n, part);
    }
    return n;
}

void path_of(const Node* n, char* out, size_t len) {
    if (!len) return;
    if (!n || !n->parent) {
        if (len >= 2) { out[0] = '/'; out[1] = 0; } else out[0] = 0;
        return;
    }
    const Node* chain[64];
    int depth = 0;
    for (const Node* p = n; p && p->parent && depth < 64; p = p->parent) chain[depth++] = p;
    size_t pos = 0;
    for (int i = depth - 1; i >= 0; i--) {
        size_t l = strlen(chain[i]->name);
        if (pos + 1 + l + 1 > len) break;
        out[pos++] = '/';
        memcpy(out + pos, chain[i]->name, l);
        pos += l;
    }
    out[pos] = 0;
}

Node* mkdir(Node* dir, const char* name, Error* err) { return make(dir, name, true, err); }
Node* create(Node* dir, const char* name, Error* err) { return make(dir, name, false, err); }

Node* mkdirs(const char* path) {
    Node* n = g_root;
    char part[NAME_MAX + 1];
    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        size_t l = 0;
        while (path[l] && path[l] != '/') l++;
        if (l > NAME_MAX) return nullptr;
        memcpy(part, path, l);
        part[l] = 0;
        path += l;
        if (l == 1 && part[0] == '.') continue;
        Node* c = find_child(n, part);
        if (!c) c = mkdir(n, part);
        if (!c || !c->dir) return nullptr;
        n = c;
    }
    return n;
}

Error write(Node* f, const void* data, size_t len) {
    if (!f) return NotFound;
    if (f->dir) return IsDir;
    if (f->ops) {
        Error e = f->ops->write(f, data, len);
        if (e != Ok) return e;
        if (!f->loaded) g_used += f->size;  // set_data subtracts the old size
        f->loaded = true;
        return set_data(f, data, len) ? Ok : NoMemory;
    }
    if (len > f->cap) {
        void* p = kmalloc(len);
        if (!p) return NoMemory;
        kfree(f->data);
        f->data = static_cast<uint8_t*>(p);
        f->cap = len;
    }
    if (len) memcpy(f->data, data, len);
    g_used = g_used - f->size + len;
    f->size = len;
    f->mtime_ms = apic::uptime_ms();
    return Ok;
}

Error append(Node* f, const void* data, size_t len) {
    if (!f) return NotFound;
    if (f->dir) return IsDir;
    if (f->ops) {
        if (!ensure_data(f)) return IoError;
        auto* tmp = static_cast<uint8_t*>(kmalloc(f->size + len + 1));
        if (!tmp) return NoMemory;
        memcpy(tmp, f->data, f->size);
        memcpy(tmp + f->size, data, len);
        Error e = write(f, tmp, f->size + len);
        kfree(tmp);
        return e;
    }
    if (f->size + len > f->cap) {
        size_t cap = f->cap ? f->cap : 256;
        while (cap < f->size + len) cap *= 2;
        void* p = krealloc(f->data, cap);
        if (!p) return NoMemory;
        f->data = static_cast<uint8_t*>(p);
        f->cap = cap;
    }
    memcpy(f->data + f->size, data, len);
    f->size += len;
    g_used += len;
    f->mtime_ms = apic::uptime_ms();
    return Ok;
}

Error remove(Node* n) {
    if (!n) return NotFound;
    if (n == g_root) return Busy;
    bool is_mount = n->ops && n->parent == g_root;
    if (n->ops && !is_mount) {
        if (n->dir) {
            if (!ensure_dir(n)) return IoError;
            while (n->child) {
                Error e = remove(n->child);
                if (e != Ok) return e;
            }
        }
        Error e = n->ops->remove(n);
        if (e != Ok) return e;
    }
    unlink(n);
    free_tree(n);
    return Ok;
}

bool is_ancestor(const Node* a, const Node* b) {
    for (const Node* p = b; p; p = p->parent)
        if (p == a) return true;
    return false;
}

Error copy(Node* src, Node* dst_dir, const char* name, Node** out) {
    if (!src) return NotFound;
    if (!dst_dir || !dst_dir->dir) return NotDir;
    if (src->dir && is_ancestor(src, dst_dir)) return Recursive;
    if (src->dir ? !ensure_dir(src) : !ensure_data(src)) return IoError;
    Error e;
    Node* n = make(dst_dir, name, src->dir, &e);
    if (!n) return e;
    if (out) *out = n;
    if (!src->dir) return write(n, src->data, src->size);
    for (Node* c = src->child; c; c = c->next) {
        if (c == n) continue;  // copying into a sibling position of itself
        Error ce = copy(c, n, c->name);
        if (ce != Ok) return ce;
    }
    return Ok;
}

Error move(Node* src, Node* dst_dir, const char* name) {
    if (!src) return NotFound;
    if (src == g_root) return Busy;
    if (!dst_dir || !dst_dir->dir) return NotDir;
    if (src->dir && is_ancestor(src, dst_dir)) return Recursive;
    if (!valid_name(name)) return BadName;
    Node* existing = find_child(dst_dir, name);
    if (existing && existing != src) return Exists;
    if (src->ops || dst_dir->ops) {
        if (existing == src) return Ok;
        if (src->ops && src->parent == g_root) return Busy;  // mount point
        // Disk involved: copy, then delete the source.
        Error e = copy(src, dst_dir, name);
        if (e != Ok) return e;
        return remove(src);
    }
    unlink(src);
    size_t l = strlen(name);
    memmove(src->name, name, l);
    src->name[l] = 0;
    link(dst_dir, src);
    return Ok;
}

int count_children(const Node* dir) {
    if (dir) ensure_dir(const_cast<Node*>(dir));
    int n = 0;
    for (const Node* c = dir ? dir->child : nullptr; c; c = c->next) n++;
    return n;
}

uint64_t tree_size(const Node* n) {
    if (!n) return 0;
    if (!n->dir) return n->size;
    uint64_t s = 0;
    for (const Node* c = n->child; c; c = c->next) s += tree_size(c);
    return s;
}

uint64_t used_bytes() { return g_used; }
uint64_t file_count() { return g_files; }

Node* put_file(const char* path, const void* data, size_t len) {
    const char* slash = path;
    for (const char* p = path; *p; p++)
        if (*p == '/') slash = p;
    char dir[PATH_MAX];
    size_t dl = size_t(slash - path);
    if (dl >= sizeof(dir)) return nullptr;
    memcpy(dir, path, dl);
    dir[dl] = 0;
    Node* d = dl ? mkdirs(dir) : g_root;
    if (!d) return nullptr;
    const char* name = (*slash == '/') ? slash + 1 : slash;
    Node* f = find_child(d, name);
    if (!f) f = create(d, name);
    if (!f || f->dir) return nullptr;
    return write(f, data, len) == Ok ? f : nullptr;
}

int load_tar(const void* data, size_t len, const char* mount_at) {
    auto* p = static_cast<const uint8_t*>(data);
    int count = 0;
    size_t off = 0;
    Node* base = mkdirs(mount_at);
    if (!base) return 0;
    char full[PATH_MAX];
    while (off + 512 <= len) {
        const uint8_t* h = p + off;
        if (h[0] == 0) break;  // end of archive
        if (memcmp(h + 257, "ustar", 5) != 0) break;
        uint64_t size = 0;
        for (int i = 124; i < 136 && h[i] >= '0' && h[i] <= '7'; i++) size = size * 8 + (h[i] - '0');
        char type = char(h[156]);
        // name = prefix + "/" + name
        size_t pos = 0;
        size_t mlen = strlen(mount_at);
        memcpy(full, mount_at, mlen);
        pos = mlen;
        if (h[345]) {
            full[pos++] = '/';
            for (int i = 0; i < 155 && h[345 + i]; i++) full[pos++] = char(h[345 + i]);
        }
        full[pos++] = '/';
        for (int i = 0; i < 100 && h[i]; i++) full[pos++] = char(h[i]);
        full[pos] = 0;
        // strip trailing slash
        while (pos > 1 && full[pos - 1] == '/') full[--pos] = 0;

        size_t data_off = off + 512;
        if (data_off + size > len) break;
        if (type == '5') {
            if (mkdirs(full)) count++;
        } else if (type == '0' || type == 0) {
            if (put_file(full, p + data_off, size_t(size))) count++;
        }
        off = data_off + ((size + 511) & ~511ull);
    }
    return count;
}

}  // namespace fs
