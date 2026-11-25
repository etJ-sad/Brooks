// SPDX-License-Identifier: GPL-2.0-only
// In-memory file system (tmpfs). Populated at boot from the initrd (USTAR
// archive loaded by Limine). Changes live in RAM until a disk driver exists.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace fs {

constexpr int NAME_MAX = 120;
constexpr int PATH_MAX = 1024;

struct Node;
enum Error : int;

// Backend hooks for nodes that live on a disk file system (nullptr = pure RAM).
struct Ops {
    bool (*load_dir)(Node* dir);    // populate children with fs::attach()
    bool (*load_file)(Node* f);     // fill data with fs::set_data()
    Error (*create)(Node* dir, Node* n);
    Error (*write)(Node* f, const void* data, size_t len);
    Error (*remove)(Node* n);       // children are already removed
};

struct Node {
    char name[NAME_MAX + 1];
    bool dir;
    Node* parent;
    Node* child;  // first child, kept sorted: directories first, then by name
    Node* next;
    uint8_t* data;
    size_t size, cap;
    uint64_t mtime_ms;
    // disk backend
    const Ops* ops;
    void* mnt;
    uint64_t fs_a, fs_b, fs_c;  // backend-private (FAT: first cluster, entry index, LFN count)
    bool loaded;                // children / data present in RAM
};

enum Error : int { Ok = 0, NotFound, Exists, NotDir, IsDir, NoMemory, BadName, Recursive, Busy, ReadOnly, IoError, NoSpace };
const char* error_str(Error e);

void init();
Node* root();
Node* lookup(const char* path);  // absolute path
Node* find_child(Node* dir, const char* name);
void path_of(const Node* n, char* out, size_t len);

Node* mkdir(Node* dir, const char* name, Error* err = nullptr);
Node* create(Node* dir, const char* name, Error* err = nullptr);
Node* mkdirs(const char* path);  // like mkdir -p, returns last dir
Error write(Node* f, const void* data, size_t len);  // replace contents
Error append(Node* f, const void* data, size_t len);
Error remove(Node* n);  // recursive
Error copy(Node* src, Node* dst_dir, const char* name, Node** out = nullptr);  // recursive
Error move(Node* src, Node* dst_dir, const char* name);  // move and/or rename

bool is_ancestor(const Node* a, const Node* b);  // a is b or contains b
int count_children(const Node* dir);
uint64_t tree_size(const Node* n);  // bytes of all files below n
uint64_t used_bytes();
uint64_t file_count();

// Make sure a disk directory's children / a disk file's data are in RAM.
bool ensure_dir(Node* dir);
bool ensure_data(Node* f);
// Backend helpers: add a child without calling back into the backend, and set
// cached file contents.
Node* attach(Node* dir, const char* name, bool is_dir);
bool set_data(Node* f, const void* data, size_t len);
// Create a mount point `name` under root served by `ops`.
Node* mount(const char* name, const Ops* ops, void* mnt, uint64_t a, uint64_t b, uint64_t c);

// Populate from a USTAR archive; returns number of entries added.
int load_tar(const void* data, size_t len, const char* mount_at = "/");
// Convenience: create or replace a file with the given text.
Node* put_file(const char* path, const void* data, size_t len);

}  // namespace fs
