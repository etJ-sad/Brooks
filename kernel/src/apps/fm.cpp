// SPDX-License-Identifier: GPL-2.0-only
// Two-panel file manager. Keys: F1..F10, or digits 1..0 on laptops where the
// F-row needs Fn.
#include "acpi/acpi.h"
#include "efi/efi.h"
#include "npu/npu.h"
#include "apps/apps.h"
#include "apps/ui.h"
#include "arch/x86_64/apic.h"
#include "drivers/keyboard.h"
#include "fs/storage.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "version.h"

namespace apps {

using namespace con;

namespace {

struct Panel {
    fs::Node* dir;
    fs::Node** items;  // items[0] == nullptr means ".."
    int count, cap;
    int sel, top;
};

Panel g_p[2];
int g_active = 0;
char g_msg[256] = "";
con::Color g_msg_color = Default;

const char* const kKeys[10] = {"Помощь", "Лог", "Смотр", "Правка", "Копия", "Перенос", "Папка", "Удалить", "Новый", "Меню"};

void set_msg(con::Color c, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void set_msg(con::Color c, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(g_msg, sizeof(g_msg), fmt, ap);
    va_end(ap);
    g_msg_color = c;
}

fs::Node* selected(Panel& p);

// Rebuild the item list. `keep`: node to keep selected; nullptr keeps the index.
void reload(Panel& p, fs::Node* keep) {
    int n = fs::count_children(p.dir) + 1;
    if (n > p.cap) {
        kfree(p.items);
        p.cap = n + 16;
        p.items = static_cast<fs::Node**>(kmalloc(sizeof(fs::Node*) * size_t(p.cap)));
    }
    p.count = 0;
    if (p.dir->parent) p.items[p.count++] = nullptr;
    for (fs::Node* c = p.dir->child; c; c = c->next) p.items[p.count++] = c;
    if (keep)
        for (int i = 0; i < p.count; i++)
            if (p.items[i] == keep) p.sel = i;
    if (p.sel >= p.count) p.sel = p.count ? p.count - 1 : 0;
    if (p.sel < 0) p.sel = 0;
}

// After operations that do not free nodes (copy, move, edit).
void reload_all() {
    for (Panel& p : g_p) reload(p, selected(p));
}

fs::Node* selected(Panel& p) { return p.count ? p.items[p.sel] : nullptr; }

void draw_topbar() {
    char clock[64], buf[256], fsz[32], ram[32];
    ui::clock_str(clock, sizeof(clock));
    ui::fmt_size(fs::used_bytes(), fsz, sizeof(fsz));
    ui::fmt_size(mm::free_bytes(), ram, sizeof(ram));
    ksnprintf(buf, sizeof(buf), "%s %s  Файлы   ФС в RAM: %s, %lu файлов   свободно RAM: %s", OS_NAME, OS_VERSION, fsz,
              fs::file_count(), ram);
    ui::topbar(buf);
    int cl = utf8_cols(clock);
    text(ui::cols() - cl - 1, 0, clock, Yellow, BgBar);
}

void draw_panel(int idx) {
    Panel& p = g_p[idx];
    const int W = ui::cols(), H = ui::rows();
    const int pw = W / 2, x = idx * pw, w = (idx == 1) ? W - pw : pw;
    const int y = 1, h = H - 3;
    const bool active = idx == g_active;
    char path[fs::PATH_MAX];
    fs::path_of(p.dir, path, sizeof(path));
    ui::box(x, y, w, h, Cyan, BgPanel, path, active);

    const int inner = w - 2, list_h = h - 2;
    const int size_w = 10, name_w = inner - size_w - 1;
    if (p.sel < p.top) p.top = p.sel;
    if (p.sel >= p.top + list_h) p.top = p.sel - list_h + 1;
    if (p.top < 0) p.top = 0;

    for (int r = 0; r < list_h; r++) {
        int i = p.top + r;
        int ry = y + 1 + r;
        if (i >= p.count) break;
        fs::Node* n = p.items[i];
        bool is_sel = i == p.sel;
        Color bg = is_sel ? (active ? BgSelect : BgInactiveSel) : BgPanel;
        Color fg = !n ? White : n->dir ? White : Default;
        if (is_sel) fg = White;
        fill(x + 1, ry, inner, 1, ' ', fg, bg);
        char sz[32];
        if (!n) {
            text(x + 1, ry, "/..", fg, bg, name_w);
            text(x + 1 + name_w + 1, ry, "  <ВВЕРХ>", Dim, bg, size_w);
            continue;
        }
        if (n->dir) {
            cell(x + 1, ry, '/', is_sel ? White : Cyan, bg);
            text(x + 2, ry, n->name, fg, bg, name_w - 1);
            ksnprintf(sz, sizeof(sz), "%9s", "<ПАПКА>");
        } else {
            text(x + 2, ry, n->name, fg, bg, name_w - 1);
            char s2[24];
            ui::fmt_size(n->size, s2, sizeof(s2));
            ksnprintf(sz, sizeof(sz), "%9s", s2);
        }
        if (utf8_cols(n->name) > name_w - 1) cell(x + name_w, ry, 0x2026, Yellow, bg);
        text(x + 1 + name_w + 1, ry, sz, is_sel ? White : Dim, bg, size_w);
    }
}

void draw_status() {
    const int W = ui::cols(), y = ui::rows() - 2;
    fill(0, y, W, 1, ' ', Default, Black);
    if (g_msg[0]) {
        text(1, y, g_msg, g_msg_color, Black, W - 2);
        return;
    }
    Panel& p = g_p[g_active];
    fs::Node* n = selected(p);
    char buf[300], sz[32];
    if (!n) {
        ksnprintf(buf, sizeof(buf), "Enter: на уровень выше");
    } else if (n->dir) {
        ui::fmt_size(fs::tree_size(n), sz, sizeof(sz));
        ksnprintf(buf, sizeof(buf), "%s   папка, %d элементов, %s", n->name, fs::count_children(n), sz);
    } else {
        ksnprintf(buf, sizeof(buf), "%s   %lu байт   изменён через %lu с после загрузки", n->name, n->size,
                  n->mtime_ms / 1000);
    }
    text(1, y, buf, Default, Black, W - 2);
}

void draw() {
    draw_topbar();
    draw_panel(0);
    draw_panel(1);
    draw_status();
    ui::keybar(kKeys);
    flush();
}

void idle() { draw_topbar(); }

// Resolve "name", "dir/name" or "/abs/path" into (directory, final name),
// relative to `base`. An existing directory as target means "into it, keep name".
bool resolve_target(const char* in, fs::Node* base, const char* keep_name, fs::Node** dir, char* name) {
    char buf[fs::PATH_MAX];
    size_t l = strlen(in);
    if (l == 0 || l >= sizeof(buf)) return false;
    memcpy(buf, in, l + 1);
    while (l > 1 && buf[l - 1] == '/') buf[--l] = 0;
    fs::Node* start = buf[0] == '/' ? fs::root() : base;
    // whole path is an existing directory?
    fs::Node* whole = nullptr;
    if (buf[0] == '/') whole = fs::lookup(buf);
    else {
        char full[fs::PATH_MAX], bp[fs::PATH_MAX];
        fs::path_of(base, bp, sizeof(bp));
        ksnprintf(full, sizeof(full), "%s/%s", bp, buf);
        whole = fs::lookup(full);
    }
    if (whole && whole->dir) {
        *dir = whole;
        memcpy(name, keep_name, strlen(keep_name) + 1);
        return true;
    }
    char* slash = nullptr;
    for (char* p = buf; *p; p++)
        if (*p == '/') slash = p;
    if (!slash) {
        if (l > size_t(fs::NAME_MAX)) return false;  // name[] holds NAME_MAX + 1 bytes
        *dir = start;
        memcpy(name, buf, l + 1);
        return true;
    }
    *slash = 0;
    const char* dpart = buf;
    fs::Node* d;
    if (buf[0] == 0) d = fs::root();
    else if (buf[0] == '/') d = fs::lookup(dpart);
    else {
        char full[fs::PATH_MAX], bp[fs::PATH_MAX];
        fs::path_of(base, bp, sizeof(bp));
        ksnprintf(full, sizeof(full), "%s/%s", bp, dpart);
        d = fs::lookup(full);
    }
    if (!d || !d->dir) return false;
    *dir = d;
    size_t nl = strlen(slash + 1);
    if (nl > size_t(fs::NAME_MAX)) return false;
    memcpy(name, slash + 1, nl + 1);
    return nl > 0;
}

void do_copy_move(bool move) {
    Panel& src = g_p[g_active];
    Panel& dst = g_p[1 - g_active];
    fs::Node* n = selected(src);
    if (!n) return;
    char target[fs::PATH_MAX], dpath[fs::PATH_MAX];
    fs::path_of(dst.dir, dpath, sizeof(dpath));
    if (dst.dir == src.dir) ksnprintf(target, sizeof(target), move ? "%s" : "копия-%s", n->name);
    else ksnprintf(target, sizeof(target), "%s/", dpath[1] ? dpath : "");
    char prompt[300];
    ksnprintf(prompt, sizeof(prompt), "%s «%s» в (папка или новое имя):", move ? "Перенести" : "Копировать", n->name);
    if (!ui::input(move ? "Перенос / переименование" : "Копирование", prompt, target, sizeof(target))) return;
    fs::Node* d;
    char name[fs::NAME_MAX + 1];
    if (!resolve_target(target, src.dir, n->name, &d, name)) {
        set_msg(Red, "Неверный путь: %s", target);
        return;
    }
    // A move that touches a disk is copy + delete: the source nodes are freed,
    // so panels inside the moved folder go up to its parent first.
    if (move && n->dir && (n->ops || d->ops))
        for (Panel& q : g_p)
            if (fs::is_ancestor(n, q.dir)) {
                q.dir = n->parent;
                q.sel = 0;
                q.top = 0;
                reload(q, nullptr);
            }
    fs::Node* out = nullptr;
    fs::Error e = move ? fs::move(n, d, name) : fs::copy(n, d, name, &out);
    if (e != fs::Ok) {
        set_msg(Red, "%s: %s", move ? "Перенос не удался" : "Копирование не удалось", fs::error_str(e));
        return;
    }
    set_msg(Green, "%s: %s", move ? "Перенесено" : "Скопировано", name);
    reload_all();
}

void do_mkdir(bool file) {
    Panel& p = g_p[g_active];
    char name[fs::NAME_MAX + 1] = "";
    if (!ui::input(file ? "Новый файл" : "Новая папка", "Имя:", name, sizeof(name))) return;
    fs::Error e;
    fs::Node* n = file ? fs::create(p.dir, name, &e) : fs::mkdir(p.dir, name, &e);
    if (!n) {
        set_msg(Red, "Не удалось создать «%s»: %s", name, fs::error_str(e));
        return;
    }
    reload(p, n);
    if (file) {
        edit(n);
        reload_all();
    }
}

void do_delete() {
    Panel& p = g_p[g_active];
    fs::Node* n = selected(p);
    if (!n) return;
    char q[300];
    if (n->dir) ksnprintf(q, sizeof(q), "Удалить папку «%s» и всё внутри (%d элементов)?", n->name, fs::count_children(n));
    else ksnprintf(q, sizeof(q), "Удалить файл «%s»?", n->name);
    if (!ui::confirm("Удаление", q)) return;
    char name[fs::NAME_MAX + 1];
    memcpy(name, n->name, sizeof(name));
    // Move panels out of the doomed subtree and forget pointers into it first.
    fs::Node* keep[2];
    for (int i = 0; i < 2; i++) {
        Panel& q2 = g_p[i];
        if (fs::is_ancestor(n, q2.dir)) {
            q2.dir = n->parent;
            q2.sel = 0;
            q2.top = 0;
            keep[i] = nullptr;
        } else {
            fs::Node* s2 = selected(q2);
            keep[i] = (s2 && fs::is_ancestor(n, s2)) ? nullptr : s2;
        }
    }
    fs::Error e = fs::remove(n);
    if (e != fs::Ok) {
        set_msg(Red, "Не удалось удалить: %s", fs::error_str(e));
        return;
    }
    for (int i = 0; i < 2; i++) reload(g_p[i], keep[i]);
    set_msg(Green, "Удалено: %s", name);
}

void show_log() {
    fs::Node* f = fs::put_file("/sys/bootlog.txt", log_data(), log_size());
    if (f) view(f);
    reload_all();
}

void show_help() {
    static const char* const lines[] = {
        "Файловый менеджер. F-клавиши работают и цифрами 1..0 (на ноутбуке F-ряд часто требует Fn).",
        "",
        "Стрелки, PgUp/PgDn, Home/End   перемещение",
        "Enter                          открыть папку / посмотреть файл",
        "Backspace                      на уровень выше",
        "Tab                            другая панель",
        "1 / F1   эта справка          2 / F2   лог загрузки",
        "3 / F3   просмотр (h: hex)     4 / F4   редактор (F2 или Ctrl+S: сохранить)",
        "5 / F5   копировать            6 / F6   перенести / переименовать",
        "7 / F7   новая папка           8 / F8 / Delete   удалить",
        "9 / F9   новый файл            0 / F10  меню: USB, перезагрузка, выключение",
        "U        подключить USB-накопители (FAT32, exFAT), они появятся как /usb0, /usb1 ...",
        "W        разрешить / запретить запись на накопитель текущей панели",
        "",
        "Корень / живёт в RAM: изменения там пропадут после перезагрузки.",
        "Файлы на /usbN пишутся на диск сразу. Загрузочный USB-SSD доступен для записи,",
        "остальные накопители только для чтения: W разрешает запись (или параметр usbrw=all).",
    };
    const int n = int(sizeof(lines) / sizeof(lines[0]));
    int w = 0;
    for (auto* l : lines) w = utf8_cols(l) > w ? utf8_cols(l) : w;
    w += 6;
    if (w > ui::cols() - 2) w = ui::cols() - 2;
    int h = n + 4;
    int x = (ui::cols() - w) / 2, y = (ui::rows() - h) / 2;
    ui::box(x, y, w, h, White, BgDialog, "Справка");
    for (int i = 0; i < n; i++) text(x + 3, y + 2 + i, lines[i], White, BgDialog, w - 6);
    flush();
    ui::next_key();
}

void usb_log(const char* line) {
    kprintf("%s\n", line);  // boot log + serial (screen output is suspended)
    ui::logbox_add(line);
}

}  // namespace

bool g_usb_all_rw = false;

namespace {

void attach_usb() {
    ui::logbox_begin("USB-накопители");
    ui::logbox_add("Внимание: драйвер USB забирает контроллер у BIOS, USB-клавиатура может перестать работать.");
    int n = storage::attach(usb_log, g_usb_all_rw);
    char line[96];
    ksnprintf(line, sizeof(line), "Подключено томов: %d. Они видны в корне как /usb0, /usb1 ...", n);
    ui::logbox_add(line);
    ui::logbox_end();
    for (Panel& p : g_p) reload(p, selected(p));
}

void npu_log(const char* line) { ui::logbox_add(line); }

void npu_test() {
    ui::logbox_begin("NPU: питание и запуск прошивки (шаги A и B)");
    npu::bringup(npu_log, true);
    ui::logbox_add("Полный лог: /sys/npu/bringup.txt");
    ui::logbox_end();
    for (Panel& p : g_p) reload(p, selected(p));
}

void toggle_write(Panel& p) {
    fs::MountBase* m = fs::mount_of(p.dir);
    if (!m) {
        set_msg(Yellow, "Эта папка в RAM, запись и так разрешена.");
        return;
    }
    if (!m->can_write) {
        set_msg(Yellow, "Эта файловая система пока поддерживается только для чтения.");
        return;
    }
    if (m->ro) {
        if (!ui::confirm("Запись на диск", "Разрешить запись на этот накопитель? Драйвер новый: сделайте копию важных данных."))
            return;
        m->ro = false;
        set_msg(Green, "Запись разрешена.");
    } else {
        m->ro = true;
        set_msg(Green, "Накопитель снова только для чтения.");
    }
}

void power_menu() {
    char c = ui::choose("Меню",
                        "U: подключить USB-накопители   R: перезагрузка   P: выключение   "
                        "F: перезагрузка в настройки UEFI (BIOS)   N: тест NPU   Esc: отмена",
                        "urpfn");
    if (c == 'n') npu_test();
    if (c == 'u') attach_usb();
    if (c == 'r') {
        efi::reset(efi::Cold);  // returns only if the firmware refused
        acpi::reboot();
    }
    if (c == 'p') {
        efi::reset(efi::Shutdown);
        acpi::poweroff();
        ui::message("Выключение", "Ни UEFI, ни ACPI S5 не выключили машину. Выключите кнопкой питания.");
    }
    if (c == 'f') {
        if (!efi::request_firmware_setup()) {
            ui::message("Настройки UEFI", efi::available() ? "Прошивка не поддерживает вход в настройки по запросу ОС."
                                                           : "UEFI runtime services недоступны.");
            return;
        }
        efi::reset(efi::Cold);
        acpi::reboot();
    }
}

void enter(Panel& p) {
    fs::Node* n = selected(p);
    if (!n) {  // ".." (or an empty root panel: nothing to do)
        if (!p.dir || !p.dir->parent) return;
        fs::Node* from = p.dir;
        p.dir = p.dir->parent;
        p.sel = 0;
        p.top = 0;
        reload(p, from);
        return;
    }
    if (n->dir) {
        p.dir = n;
        p.sel = 0;
        p.top = 0;
        reload(p, nullptr);
        return;
    }
    view(n);
}

}  // namespace

void file_manager(bool usb_auto) {
    suspend_stream(true);
    fs::Node* home = fs::lookup("/home");
    g_p[0] = {home ? home : fs::root(), nullptr, 0, 0, 0, 0};
    g_p[1] = {fs::root(), nullptr, 0, 0, 0, 0};
    reload(g_p[0], nullptr);
    reload(g_p[1], nullptr);
    fill(0, 0, ui::cols(), ui::rows(), ' ', Default, Black);
    set_msg(Dim, "1 или F1: справка. U: подключить USB-накопители. Tab: другая панель.");
    if (usb_auto) {
        draw();
        attach_usb();
    }

    for (;;) {
        draw();
        kbd::Event e = ui::next_key(idle);
        g_msg[0] = 0;
        Panel& p = g_p[g_active];
        const int list_h = ui::rows() - 5;
        kbd::Key k = e.key;
        if (k == kbd::Char && !(e.mods & (kbd::Ctrl | kbd::Alt))) {
            if (e.ch >= '1' && e.ch <= '9') k = kbd::Key(kbd::F1 + (e.ch - '1'));
            else if (e.ch == '0') k = kbd::F10;
        }
        switch (k) {
        case kbd::Up: if (p.sel > 0) p.sel--; break;
        case kbd::Down: if (p.sel + 1 < p.count) p.sel++; break;
        case kbd::PageUp: p.sel = p.sel > list_h ? p.sel - list_h : 0; break;
        case kbd::PageDown: p.sel = p.sel + list_h < p.count ? p.sel + list_h : p.count - 1; break;
        case kbd::Home: p.sel = 0; break;
        case kbd::End: p.sel = p.count - 1; break;
        case kbd::Tab: g_active = 1 - g_active; break;
        case kbd::Enter: enter(p); break;
        case kbd::Backspace:
            if (p.dir->parent) {
                p.sel = 0;
                if (p.count && !p.items[0]) enter(p);
            }
            break;
        case kbd::F1: show_help(); break;
        case kbd::F2: show_log(); break;
        case kbd::F3: if (fs::Node* n = selected(p); n && !n->dir) view(n); break;
        case kbd::F4:
            if (fs::Node* n = selected(p); n && !n->dir) {
                edit(n);
                reload_all();
            }
            break;
        case kbd::F5: do_copy_move(false); break;
        case kbd::F6: do_copy_move(true); break;
        case kbd::F7: do_mkdir(false); break;
        case kbd::F8: case kbd::Delete: do_delete(); break;
        case kbd::F9: do_mkdir(true); break;
        case kbd::F10: power_menu(); break;
        case kbd::Char:
            if (e.ch == 'u' || e.ch == 'U') attach_usb();
            if (e.ch == 'w' || e.ch == 'W') toggle_write(p);
            break;
        default: break;
        }
    }
}

}  // namespace apps
