/*
    B++ Setup

    Native Windows setup wizard for the C version of B++.
    This installer does not use another scripting language.
*/

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

#include "bpp_version.h"

#define APP_CLASS "BppSetupWindow"
#define INSTALL_NAME "B++ Setup"
#define PATH_LIMIT 4096

#define IDC_PATH_EDIT 1001
#define IDC_BROWSE 1002
#define IDC_ADD_PATH 1003
#define IDC_REGISTER_BPP 1004
#define IDC_ALIAS 1005
#define IDC_INSTALL 1006
#define IDC_UNINSTALL 1007
#define IDC_CANCEL 1008
#define IDC_STATUS 1009

static HWND g_path_edit;
static HWND g_status;

static void set_status(const char *text) {
    SetWindowTextA(g_status, text);
}

static void show_error(HWND hwnd, const char *text) {
    MessageBoxA(hwnd, text, INSTALL_NAME, MB_ICONERROR | MB_OK);
}

static void show_info(HWND hwnd, const char *text) {
    MessageBoxA(hwnd, text, INSTALL_NAME, MB_ICONINFORMATION | MB_OK);
}

static int file_exists(const char *path) {
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static void path_dirname(const char *path, char *out, size_t out_size) {
    size_t len = strlen(path);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, path, len);
    out[len] = '\0';

    for (char *p = out + strlen(out); p > out; p--) {
        if (*p == '\\' || *p == '/') {
            *p = '\0';
            return;
        }
    }
    strcpy(out, ".");
}

static void path_join(char *out, size_t out_size, const char *left, const char *right) {
    size_t len = strlen(left);
    if (len > 0 && (left[len - 1] == '\\' || left[len - 1] == '/')) {
        snprintf(out, out_size, "%s%s", left, right);
    } else {
        snprintf(out, out_size, "%s\\%s", left, right);
    }
}

static void default_install_path(char *out, size_t out_size) {
    DWORD got = GetEnvironmentVariableA("LOCALAPPDATA", out, (DWORD)out_size);
    if (got == 0 || got >= out_size) {
        GetEnvironmentVariableA("USERPROFILE", out, (DWORD)out_size);
        strncat(out, "\\AppData\\Local", out_size - strlen(out) - 1);
    }
    strncat(out, "\\Bpp", out_size - strlen(out) - 1);
}

static int ensure_directory(const char *path) {
    char work[PATH_LIMIT];
    snprintf(work, sizeof(work), "%s", path);

    size_t len = strlen(work);
    if (len == 0) {
        return 0;
    }

    for (char *p = work; *p; p++) {
        if (*p == '/' || *p == '\\') {
            if (p == work || (p == work + 2 && work[1] == ':')) {
                continue;
            }

            char old = *p;
            *p = '\0';
            if (strlen(work) > 0) {
                CreateDirectoryA(work, NULL);
            }
            *p = old;
        }
    }

    if (!CreateDirectoryA(work, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            return 0;
        }
    }

    return 1;
}

static void trim_trailing_slashes(char *text) {
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\\' || text[len - 1] == '/')) {
        text[--len] = '\0';
    }
}

static int same_path_text(const char *a, const char *b) {
    char left[PATH_LIMIT];
    char right[PATH_LIMIT];
    snprintf(left, sizeof(left), "%s", a);
    snprintf(right, sizeof(right), "%s", b);
    trim_trailing_slashes(left);
    trim_trailing_slashes(right);
    return _stricmp(left, right) == 0;
}

static int list_has_segment(const char *list, const char *segment) {
    char work[PATH_LIMIT * 2];
    snprintf(work, sizeof(work), "%s", list ? list : "");

    char *start = work;
    while (start && *start) {
        char *end = strchr(start, ';');
        if (end) {
            *end = '\0';
        }

        while (*start == ' ') {
            start++;
        }

        if (*start && same_path_text(start, segment)) {
            return 1;
        }

        start = end ? end + 1 : NULL;
    }

    return 0;
}

static void remove_path_segment(const char *list, const char *segment, char *out, size_t out_size) {
    char work[PATH_LIMIT * 2];
    snprintf(work, sizeof(work), "%s", list ? list : "");
    out[0] = '\0';

    char *start = work;
    while (start && *start) {
        char *end = strchr(start, ';');
        if (end) {
            *end = '\0';
        }

        while (*start == ' ') {
            start++;
        }

        if (*start && !same_path_text(start, segment)) {
            if (out[0]) {
                strncat(out, ";", out_size - strlen(out) - 1);
            }
            strncat(out, start, out_size - strlen(out) - 1);
        }

        start = end ? end + 1 : NULL;
    }
}

static int env_get_user(const char *name, char *out, size_t out_size) {
    HKEY key;
    DWORD type = 0;
    DWORD size = (DWORD)out_size;
    out[0] = '\0';

    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Environment", 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return 0;
    }

    LONG rc = RegQueryValueExA(key, name, NULL, &type, (BYTE *)out, &size);
    RegCloseKey(key);

    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        out[0] = '\0';
        return 0;
    }

    out[out_size - 1] = '\0';
    return 1;
}

static int env_set_user(const char *name, const char *value) {
    HKEY key;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Environment", 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS) {
        return 0;
    }

    LONG rc = RegSetValueExA(key, name, 0, REG_SZ, (const BYTE *)value, (DWORD)strlen(value) + 1);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

static void broadcast_environment_change(void) {
    SendMessageTimeoutA(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)"Environment",
                        SMTO_ABORTIFHUNG, 5000, NULL);
}

static int add_user_path(const char *path) {
    char current[PATH_LIMIT * 2];
    char next[PATH_LIMIT * 2];
    env_get_user("Path", current, sizeof(current));

    if (list_has_segment(current, path)) {
        return 1;
    }

    if (current[0]) {
        snprintf(next, sizeof(next), "%s;%s", current, path);
    } else {
        snprintf(next, sizeof(next), "%s", path);
    }

    return env_set_user("Path", next);
}

static int remove_user_path(const char *path) {
    char current[PATH_LIMIT * 2];
    char next[PATH_LIMIT * 2];
    env_get_user("Path", current, sizeof(current));
    remove_path_segment(current, path, next, sizeof(next));
    return env_set_user("Path", next);
}

static int extension_in_pathext(const char *list, const char *ext) {
    char work[PATH_LIMIT];
    snprintf(work, sizeof(work), "%s", list ? list : "");

    char *start = work;
    while (start && *start) {
        char *end = strchr(start, ';');
        if (end) {
            *end = '\0';
        }

        while (*start == ' ') {
            start++;
        }

        if (_stricmp(start, ext) == 0) {
            return 1;
        }

        start = end ? end + 1 : NULL;
    }

    return 0;
}

static int add_user_pathext(const char *ext) {
    char current[PATH_LIMIT];
    char next[PATH_LIMIT];
    env_get_user("PATHEXT", current, sizeof(current));

    if (extension_in_pathext(current, ext)) {
        return 1;
    }

    if (current[0]) {
        snprintf(next, sizeof(next), "%s;%s", current, ext);
    } else {
        snprintf(next, sizeof(next), "%s", ext);
    }

    return env_set_user("PATHEXT", next);
}

static int remove_user_pathext(const char *ext) {
    char current[PATH_LIMIT];
    char next[PATH_LIMIT];
    char work[PATH_LIMIT];

    env_get_user("PATHEXT", current, sizeof(current));
    next[0] = '\0';

    snprintf(work, sizeof(work), "%s", current);

    char *start = work;
    while (start && *start) {
        char *end = strchr(start, ';');
        if (end) {
            *end = '\0';
        }

        while (*start == ' ') {
            start++;
        }

        if (*start && _stricmp(start, ext) != 0) {
            if (next[0]) {
                strncat(next, ";", sizeof(next) - strlen(next) - 1);
            }
            strncat(next, start, sizeof(next) - strlen(next) - 1);
        }

        start = end ? end + 1 : NULL;
    }

    return env_set_user("PATHEXT", next);
}

static int set_default_value(const char *subkey, const char *value) {
    HKEY key;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, subkey, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS) {
        return 0;
    }

    LONG rc = RegSetValueExA(key, NULL, 0, REG_SZ, (const BYTE *)value, (DWORD)strlen(value) + 1);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

static void write_le16(FILE *file, unsigned int value) {
    unsigned char bytes[2];
    bytes[0] = (unsigned char)(value & 0xff);
    bytes[1] = (unsigned char)((value >> 8) & 0xff);
    fwrite(bytes, 1, sizeof(bytes), file);
}

static void write_le32(FILE *file, unsigned int value) {
    unsigned char bytes[4];
    bytes[0] = (unsigned char)(value & 0xff);
    bytes[1] = (unsigned char)((value >> 8) & 0xff);
    bytes[2] = (unsigned char)((value >> 16) & 0xff);
    bytes[3] = (unsigned char)((value >> 24) & 0xff);
    fwrite(bytes, 1, sizeof(bytes), file);
}

static void icon_pixel(unsigned char *pixels, int width, int height, int x, int y,
                       unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return;
    }

    unsigned char *p = pixels + ((y * width + x) * 4);
    p[0] = b;
    p[1] = g;
    p[2] = r;
    p[3] = a;
}

static void icon_rect(unsigned char *pixels, int width, int height, int left, int top, int right, int bottom,
                      unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    for (int y = top; y <= bottom; y++) {
        for (int x = left; x <= right; x++) {
            icon_pixel(pixels, width, height, x, y, r, g, b, a);
        }
    }
}

static int write_bpp_file_icon(const char *path) {
    enum { ICON_W = 48, ICON_H = 48 };
    enum { XOR_SIZE = ICON_W * ICON_H * 4 };
    enum { MASK_STRIDE = ((ICON_W + 31) / 32) * 4 };
    enum { MASK_SIZE = MASK_STRIDE * ICON_H };
    enum { DIB_SIZE = 40 + XOR_SIZE + MASK_SIZE };
    unsigned char pixels[XOR_SIZE];
    unsigned char mask[MASK_SIZE];

    memset(pixels, 0, sizeof(pixels));
    memset(mask, 0, sizeof(mask));

    icon_rect(pixels, ICON_W, ICON_H, 10, 7, 38, 45, 0, 0, 0, 42);
    icon_rect(pixels, ICON_W, ICON_H, 7, 4, 35, 43, 248, 251, 255, 255);
    icon_rect(pixels, ICON_W, ICON_H, 7, 4, 35, 4, 188, 199, 214, 255);
    icon_rect(pixels, ICON_W, ICON_H, 7, 4, 7, 43, 188, 199, 214, 255);
    icon_rect(pixels, ICON_W, ICON_H, 35, 15, 35, 43, 188, 199, 214, 255);
    icon_rect(pixels, ICON_W, ICON_H, 7, 43, 35, 43, 188, 199, 214, 255);

    for (int y = 4; y <= 15; y++) {
        int x_start = 24 + (y - 4);
        for (int x = x_start; x <= 35; x++) {
            icon_pixel(pixels, ICON_W, ICON_H, x, y, 224, 235, 247, 255);
        }
    }
    for (int i = 0; i <= 11; i++) {
        icon_pixel(pixels, ICON_W, ICON_H, 24 + i, 4 + i, 166, 181, 200, 255);
    }

    icon_rect(pixels, ICON_W, ICON_H, 12, 23, 34, 39, 31, 96, 196, 255);
    icon_rect(pixels, ICON_W, ICON_H, 12, 38, 34, 39, 19, 66, 148, 255);
    icon_rect(pixels, ICON_W, ICON_H, 12, 23, 34, 23, 79, 141, 225, 255);

    icon_rect(pixels, ICON_W, ICON_H, 15, 26, 16, 36, 255, 255, 255, 255);
    icon_rect(pixels, ICON_W, ICON_H, 17, 26, 22, 27, 255, 255, 255, 255);
    icon_rect(pixels, ICON_W, ICON_H, 17, 31, 22, 32, 255, 255, 255, 255);
    icon_rect(pixels, ICON_W, ICON_H, 17, 36, 22, 37, 255, 255, 255, 255);
    icon_rect(pixels, ICON_W, ICON_H, 22, 28, 23, 30, 255, 255, 255, 255);
    icon_rect(pixels, ICON_W, ICON_H, 22, 33, 23, 35, 255, 255, 255, 255);
    icon_rect(pixels, ICON_W, ICON_H, 26, 29, 31, 30, 255, 255, 255, 255);
    icon_rect(pixels, ICON_W, ICON_H, 28, 27, 29, 32, 255, 255, 255, 255);
    icon_rect(pixels, ICON_W, ICON_H, 26, 35, 31, 36, 255, 255, 255, 255);
    icon_rect(pixels, ICON_W, ICON_H, 28, 33, 29, 38, 255, 255, 255, 255);

    FILE *file = fopen(path, "wb");
    if (!file) {
        return 0;
    }

    write_le16(file, 0);
    write_le16(file, 1);
    write_le16(file, 1);
    fputc(ICON_W, file);
    fputc(ICON_H, file);
    fputc(0, file);
    fputc(0, file);
    write_le16(file, 1);
    write_le16(file, 32);
    write_le32(file, DIB_SIZE);
    write_le32(file, 6 + 16);

    write_le32(file, 40);
    write_le32(file, ICON_W);
    write_le32(file, ICON_H * 2);
    write_le16(file, 1);
    write_le16(file, 32);
    write_le32(file, 0);
    write_le32(file, XOR_SIZE + MASK_SIZE);
    write_le32(file, 0);
    write_le32(file, 0);
    write_le32(file, 0);
    write_le32(file, 0);

    for (int y = ICON_H - 1; y >= 0; y--) {
        fwrite(pixels + (y * ICON_W * 4), 1, ICON_W * 4, file);
    }
    fwrite(mask, 1, sizeof(mask), file);

    int ok = !ferror(file);
    fclose(file);
    return ok;
}

static void refresh_file_associations(void) {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}

static int register_bpp_files(const char *bpp_exe, const char *icon_path) {
    char command[PATH_LIMIT * 2];
    char icon[PATH_LIMIT * 2];

    snprintf(command, sizeof(command), "cmd.exe /k \"\"%s\" \"%%1\" -o \"%%1.c\"\"", bpp_exe);
    snprintf(icon, sizeof(icon), "\"%s\",0", icon_path);

    int ok =
        set_default_value("Software\\Classes\\.bpp", "Bpp.Source") &&
        set_default_value("Software\\Classes\\Bpp.Source", "B++ Source File") &&
        set_default_value("Software\\Classes\\Bpp.Source\\DefaultIcon", icon) &&
        set_default_value("Software\\Classes\\Bpp.Source\\shell", "compile") &&
        set_default_value("Software\\Classes\\Bpp.Source\\shell\\compile", "Compile with B++") &&
        set_default_value("Software\\Classes\\Bpp.Source\\shell\\compile\\command", command) &&
        set_default_value("Software\\Classes\\Bpp.Source\\shell\\open", "Compile with B++") &&
        set_default_value("Software\\Classes\\Bpp.Source\\shell\\open\\command", command) &&
        set_default_value("Software\\Classes\\Bpp.Source\\shell\\edit", "Edit") &&
        set_default_value("Software\\Classes\\Bpp.Source\\shell\\edit\\command", "notepad.exe \"%1\"");

    if (ok) {
        refresh_file_associations();
    }
    return ok;
}

static void unregister_bpp_files(void) {
    RegDeleteTreeA(HKEY_CURRENT_USER, "Software\\Classes\\.bpp");
    RegDeleteTreeA(HKEY_CURRENT_USER, "Software\\Classes\\Bpp.Source");
    refresh_file_associations();
}

static int write_text_file(const char *path, const char *text) {
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(file, text, (DWORD)strlen(text), &written, NULL);
    CloseHandle(file);
    return ok;
}

static int install_bpp(HWND hwnd, const char *install_root, int add_path, int register_files, int create_alias) {
    char setup_dir[PATH_LIMIT];
    char source_bpp[PATH_LIMIT];
    char source_setup[PATH_LIMIT];
    char install_bin[PATH_LIMIT];
    char target_bpp[PATH_LIMIT];
    char target_bpp_cmd[PATH_LIMIT];
    char target_alias[PATH_LIMIT];
    char target_icon[PATH_LIMIT];
    char target_setup[PATH_LIMIT];

    set_status("Installing...");

    GetModuleFileNameA(NULL, source_setup, sizeof(source_setup));
    path_dirname(source_setup, setup_dir, sizeof(setup_dir));
    path_join(source_bpp, sizeof(source_bpp), setup_dir, "bpp.exe");

    if (!file_exists(source_bpp)) {
        show_error(hwnd, "Could not find bpp.exe next to this setup wizard. Build B++ first, then run setup again.");
        set_status("Install failed.");
        return 0;
    }

    path_join(install_bin, sizeof(install_bin), install_root, "bin");
    path_join(target_bpp, sizeof(target_bpp), install_bin, "bpp.exe");
    path_join(target_bpp_cmd, sizeof(target_bpp_cmd), install_bin, "bpp.cmd");
    path_join(target_alias, sizeof(target_alias), install_bin, "b++.cmd");
    path_join(target_icon, sizeof(target_icon), install_bin, "bpp_file.ico");
    path_join(target_setup, sizeof(target_setup), install_root, "B++ Setup.exe");

    if (!ensure_directory(install_root) || !ensure_directory(install_bin)) {
        show_error(hwnd, "Could not create the install folder.");
        set_status("Install failed.");
        return 0;
    }

    if (!CopyFileA(source_bpp, target_bpp, FALSE)) {
        show_error(hwnd, "Could not copy bpp.exe to the install folder.");
        set_status("Install failed.");
        return 0;
    }

    if (!write_text_file(target_bpp_cmd, "@echo off\r\n\"%~dp0bpp.exe\" %*\r\n")) {
        show_error(hwnd, "Could not create the bpp command shim.");
        set_status("Install failed.");
        return 0;
    }

    if (!same_path_text(source_setup, target_setup)) {
        CopyFileA(source_setup, target_setup, FALSE);
    }

    if (create_alias) {
        const char *alias_text = "@echo off\r\n\"%~dp0bpp.exe\" %*\r\n";
        if (!write_text_file(target_alias, alias_text)) {
            show_error(hwnd, "Could not create the b++ command alias.");
            set_status("Install failed.");
            return 0;
        }
    } else {
        DeleteFileA(target_alias);
    }

    if (add_path && !add_user_path(install_bin)) {
        show_error(hwnd, "Could not add B++ to your user PATH.");
        set_status("Install failed.");
        return 0;
    }

    if (register_files) {
        if (!write_bpp_file_icon(target_icon)) {
            show_error(hwnd, "Could not install the .bpp file icon.");
            set_status("Install failed.");
            return 0;
        }
        if (!register_bpp_files(target_bpp, target_icon)) {
            show_error(hwnd, "Could not register .bpp files with Windows.");
            set_status("Install failed.");
            return 0;
        }
        add_user_pathext(".BPP");
    } else {
        DeleteFileA(target_icon);
    }

    broadcast_environment_change();
    set_status("B++ installed. Open a new terminal and run: bpp --version");
    show_info(hwnd, "B++ was installed. Open a new terminal and run: bpp --version");
    return 1;
}

static int uninstall_bpp(HWND hwnd, const char *install_root) {
    char install_bin[PATH_LIMIT];
    char target_bpp[PATH_LIMIT];
    char target_bpp_cmd[PATH_LIMIT];
    char target_alias[PATH_LIMIT];
    char target_icon[PATH_LIMIT];
    char target_setup[PATH_LIMIT];
    char current_setup[PATH_LIMIT];

    set_status("Uninstalling...");

    path_join(install_bin, sizeof(install_bin), install_root, "bin");
    path_join(target_bpp, sizeof(target_bpp), install_bin, "bpp.exe");
    path_join(target_bpp_cmd, sizeof(target_bpp_cmd), install_bin, "bpp.cmd");
    path_join(target_alias, sizeof(target_alias), install_bin, "b++.cmd");
    path_join(target_icon, sizeof(target_icon), install_bin, "bpp_file.ico");
    path_join(target_setup, sizeof(target_setup), install_root, "B++ Setup.exe");
    GetModuleFileNameA(NULL, current_setup, sizeof(current_setup));

    remove_user_path(install_bin);
    remove_user_pathext(".BPP");
    unregister_bpp_files();

    DeleteFileA(target_bpp);
    DeleteFileA(target_bpp_cmd);
    DeleteFileA(target_alias);
    DeleteFileA(target_icon);

    if (!same_path_text(current_setup, target_setup)) {
        DeleteFileA(target_setup);
    }

    RemoveDirectoryA(install_bin);
    RemoveDirectoryA(install_root);

    broadcast_environment_change();
    set_status("B++ uninstalled.");
    show_info(hwnd, "B++ was uninstalled. Open a new terminal so PATH updates are refreshed.");
    return 1;
}

static void browse_install_folder(HWND hwnd) {
    BROWSEINFOA info;
    ZeroMemory(&info, sizeof(info));
    info.hwndOwner = hwnd;
    info.lpszTitle = "Choose where B++ should be installed";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST item = SHBrowseForFolderA(&info);
    if (item) {
        char path[PATH_LIMIT];
        if (SHGetPathFromIDListA(item, path)) {
            SetWindowTextA(g_path_edit, path);
        }
        CoTaskMemFree(item);
    }
}

static HWND make_control(HWND parent, const char *class_name, const char *text, DWORD style,
                         int x, int y, int w, int h, int id) {
    return CreateWindowExA(0, class_name, text, style | WS_CHILD | WS_VISIBLE,
                           x, y, w, h, parent, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
}

static void create_ui(HWND hwnd) {
    char install_path[PATH_LIMIT];
    default_install_path(install_path, sizeof(install_path));

    make_control(hwnd, "STATIC", "B++ Setup", WS_CHILD | WS_VISIBLE, 20, 18, 540, 24, 0);
    make_control(hwnd, "STATIC", "Install the native B++ compiler and .bpp toolchain | version " BPP_VERSION,
                 WS_CHILD | WS_VISIBLE, 20, 50, 540, 22, 0);

    make_control(hwnd, "STATIC", "Install location", WS_CHILD | WS_VISIBLE, 20, 100, 160, 20, 0);
    g_path_edit = make_control(hwnd, "EDIT", install_path,
                               WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                               20, 125, 470, 24, IDC_PATH_EDIT);
    make_control(hwnd, "BUTTON", "Browse", WS_TABSTOP | BS_PUSHBUTTON,
                 505, 124, 80, 26, IDC_BROWSE);

    HWND add_path = make_control(hwnd, "BUTTON", "Add B++ to user PATH", WS_TABSTOP | BS_AUTOCHECKBOX,
                                 20, 170, 360, 22, IDC_ADD_PATH);
    HWND alias = make_control(hwnd, "BUTTON", "Create b++ command alias", WS_TABSTOP | BS_AUTOCHECKBOX,
                              20, 200, 360, 22, IDC_ALIAS);
    HWND assoc = make_control(hwnd, "BUTTON", "Register .bpp files with Windows", WS_TABSTOP | BS_AUTOCHECKBOX,
                              20, 230, 360, 22, IDC_REGISTER_BPP);

    SendMessageA(add_path, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageA(alias, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageA(assoc, BM_SETCHECK, BST_CHECKED, 0);

    g_status = make_control(hwnd, "STATIC", "Ready to install B++.", WS_CHILD | WS_VISIBLE,
                            20, 285, 560, 24, IDC_STATUS);

    make_control(hwnd, "BUTTON", "Install", WS_TABSTOP | BS_DEFPUSHBUTTON,
                 305, 325, 90, 28, IDC_INSTALL);
    make_control(hwnd, "BUTTON", "Uninstall", WS_TABSTOP | BS_PUSHBUTTON,
                 405, 325, 90, 28, IDC_UNINSTALL);
    make_control(hwnd, "BUTTON", "Cancel", WS_TABSTOP | BS_PUSHBUTTON,
                 505, 325, 80, 28, IDC_CANCEL);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    (void)lparam;

    switch (msg) {
        case WM_CREATE:
            create_ui(hwnd);
            return 0;

        case WM_COMMAND: {
            int id = LOWORD(wparam);
            if (id == IDC_BROWSE) {
                browse_install_folder(hwnd);
                return 0;
            }
            if (id == IDC_INSTALL) {
                char install_path[PATH_LIMIT];
                GetWindowTextA(g_path_edit, install_path, sizeof(install_path));
                if (install_path[0] == '\0') {
                    show_error(hwnd, "Choose an install location first.");
                    return 0;
                }

                install_bpp(
                    hwnd,
                    install_path,
                    IsDlgButtonChecked(hwnd, IDC_ADD_PATH) == BST_CHECKED,
                    IsDlgButtonChecked(hwnd, IDC_REGISTER_BPP) == BST_CHECKED,
                    IsDlgButtonChecked(hwnd, IDC_ALIAS) == BST_CHECKED
                );
                return 0;
            }
            if (id == IDC_UNINSTALL) {
                char install_path[PATH_LIMIT];
                GetWindowTextA(g_path_edit, install_path, sizeof(install_path));
                uninstall_bpp(hwnd, install_path);
                return 0;
            }
            if (id == IDC_CANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line, int show_cmd) {
    (void)previous;
    (void)command_line;

    HRESULT com_result = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = APP_CLASS;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

    if (!RegisterClassA(&wc)) {
        MessageBoxA(NULL, "Could not start setup.", INSTALL_NAME, MB_ICONERROR | MB_OK);
        if (SUCCEEDED(com_result)) {
            CoUninitialize();
        }
        return 1;
    }

    HWND hwnd = CreateWindowExA(
        0,
        APP_CLASS,
        INSTALL_NAME,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        625,
        410,
        NULL,
        NULL,
        instance,
        NULL
    );

    if (!hwnd) {
        MessageBoxA(NULL, "Could not create setup window.", INSTALL_NAME, MB_ICONERROR | MB_OK);
        if (SUCCEEDED(com_result)) {
            CoUninitialize();
        }
        return 1;
    }

    ShowWindow(hwnd, show_cmd);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    if (SUCCEEDED(com_result)) {
        CoUninitialize();
    }

    return (int)msg.wParam;
}
