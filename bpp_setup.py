import ctypes
from ctypes import wintypes
import hashlib
import json
import os
import shutil
import subprocess
import sys


APP_NAME = "B++"
APP_FOLDER = "Bpp"
VERSION = "4.0"
FILE_TYPE = "Bpp.Source"
UPDATE_CONFIG_NAME = "updates.json"
DEFAULT_UPDATE_INTERVAL_HOURS = 24
UPDATE_REPO = "MrGuineaBird/BPLUSPLUS"


def resource_path(name):
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, name)


def default_install_dir():
    local_appdata = os.environ.get("LOCALAPPDATA")
    if not local_appdata:
        local_appdata = os.path.join(os.path.expanduser("~"), "AppData", "Local")
    return os.path.join(local_appdata, APP_FOLDER)


def normalize_path(path):
    return os.path.normcase(os.path.normpath(os.path.expandvars(path))).rstrip("\\/")


def split_env_list(value):
    return [part for part in value.split(os.pathsep) if part]


def add_user_env_entry(name, entry):
    import winreg

    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, "Environment") as key:
        try:
            value, reg_type = winreg.QueryValueEx(key, name)
        except FileNotFoundError:
            value = os.environ.get(name, "")
            reg_type = winreg.REG_EXPAND_SZ if "%" in value else winreg.REG_SZ

        parts = split_env_list(value)
        if name.upper() == "PATHEXT":
            exists = any(part.upper() == entry.upper() for part in parts)
        else:
            exists = any(normalize_path(part) == normalize_path(entry) for part in parts)

        if exists:
            return False

        parts.append(entry)
        new_value = os.pathsep.join(parts)
        winreg.SetValueEx(key, name, 0, reg_type, new_value)
        os.environ[name] = new_value
        return True


def broadcast_environment_change():
    try:
        hwnd_broadcast = 0xFFFF
        wm_settingchange = 0x001A
        smto_abortifhung = 0x0002
        ctypes.windll.user32.SendMessageTimeoutW(
            hwnd_broadcast,
            wm_settingchange,
            0,
            "Environment",
            smto_abortifhung,
            5000,
            None,
        )
    except Exception:
        pass


def set_default_value(key, value):
    import winreg

    winreg.SetValueEx(key, "", 0, winreg.REG_SZ, value)


def register_file_type(bpp_exe):
    import winreg

    base = r"Software\Classes"

    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, base + r"\.bpp") as key:
        set_default_value(key, FILE_TYPE)

    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, base + "\\" + FILE_TYPE) as key:
        set_default_value(key, "B++ Source File")

    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, base + "\\" + FILE_TYPE + r"\DefaultIcon") as key:
        set_default_value(key, f'"{bpp_exe}",0')

    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, base + "\\" + FILE_TYPE + r"\shell\open\command") as key:
        set_default_value(key, f'"{bpp_exe}" --quiet --run "%1" -- %*')

    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, base + "\\" + FILE_TYPE + r"\shell\compile\command") as key:
        set_default_value(key, f'"{bpp_exe}" "%1"')

    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, base + "\\" + FILE_TYPE + r"\shell\edit\command") as key:
        set_default_value(key, 'notepad.exe "%1"')


def write_update_config(install_dir, auto_update=True, interval_hours=DEFAULT_UPDATE_INTERVAL_HOURS):
    config = {
        "repo": UPDATE_REPO,
        "auto_update": bool(auto_update),
        "interval_hours": interval_hours,
        "last_checked": 0,
    }
    path = os.path.join(install_dir, UPDATE_CONFIG_NAME)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(config, handle, indent=2, sort_keys=True)
        handle.write("\n")


def write_uninstaller(install_dir):
    path = os.path.join(install_dir, "uninstall_bpp.cmd")
    script = "\r\n".join(
        [
            "@echo off",
            "setlocal",
            "echo Removing B++ file association...",
            "reg delete HKCU\\Software\\Classes\\.bpp /f >nul 2>nul",
            "reg delete HKCU\\Software\\Classes\\Bpp.Source /f >nul 2>nul",
            "echo Registry entries were removed.",
            "echo Remove this folder manually if Windows keeps files locked:",
            f'echo "{install_dir}"',
            "pause",
            "",
        ]
    )
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(script)


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def files_match(left, right):
    if not os.path.exists(left) or not os.path.exists(right):
        return False
    if os.path.getsize(left) != os.path.getsize(right):
        return False
    return file_sha256(left) == file_sha256(right)


def copy_payload(payload, target, report):
    if files_match(payload, target):
        report(f"{os.path.basename(target)} is already current")
        return

    try:
        shutil.copy2(payload, target)
    except PermissionError as exc:
        if files_match(payload, target):
            report(f"{os.path.basename(target)} is already current")
            return
        raise RuntimeError(
            f"Could not update {target}. Close any running B++ programs and run setup again."
        ) from exc


def install_bpp(
    install_dir,
    add_path=True,
    add_pathext=True,
    associate=True,
    log=None,
    auto_update=False,
):
    if os.name != "nt":
        raise RuntimeError("This installer is for Windows.")

    def report(message):
        if log:
            log(message)

    payload = resource_path("bpp.exe")
    if not os.path.exists(payload):
        raise RuntimeError("Installer payload is missing bpp.exe.")

    install_dir = os.path.abspath(os.path.expandvars(install_dir))
    bin_dir = os.path.join(install_dir, "bin")
    bpp_exe = os.path.join(bin_dir, "bpp.exe")
    bplusplus_exe = os.path.join(bin_dir, "b++.exe")

    report(f"Creating {bin_dir}")
    os.makedirs(bin_dir, exist_ok=True)

    report("Installing compiler executable")
    copy_payload(payload, bpp_exe, report)
    copy_payload(payload, bplusplus_exe, report)

    if add_path:
        report("Adding B++ to user PATH")
        add_user_env_entry("PATH", bin_dir)

    if add_pathext:
        report("Adding .BPP to PATHEXT")
        add_user_env_entry("PATHEXT", ".BPP")

    if associate:
        report("Registering .bpp files")
        register_file_type(bpp_exe)

    report("Configuring GitHub updates")
    try:
        write_update_config(install_dir, auto_update=auto_update)
    except PermissionError:
        report("Skipping update config; Windows would not allow replacing it")

    report("Writing uninstaller helper")
    try:
        write_uninstaller(install_dir)
    except PermissionError:
        report("Skipping uninstaller helper; Windows would not allow replacing it")
    broadcast_environment_change()
    return bpp_exe


if os.name == "nt":
    user32 = ctypes.windll.user32
    gdi32 = ctypes.windll.gdi32
    kernel32 = ctypes.windll.kernel32
    shell32 = ctypes.windll.shell32
    ole32 = ctypes.windll.ole32
else:
    user32 = gdi32 = kernel32 = shell32 = ole32 = None


LRESULT = ctypes.c_ssize_t
HINSTANCE = wintypes.HANDLE
HICON = wintypes.HANDLE
HCURSOR = wintypes.HANDLE
HBRUSH = wintypes.HANDLE
HMENU = wintypes.HANDLE
HGDIOBJ = wintypes.HANDLE


WNDPROC = ctypes.WINFUNCTYPE(
    LRESULT,
    wintypes.HWND,
    wintypes.UINT,
    wintypes.WPARAM,
    wintypes.LPARAM,
)


class WNDCLASSW(ctypes.Structure):
    _fields_ = [
        ("style", wintypes.UINT),
        ("lpfnWndProc", WNDPROC),
        ("cbClsExtra", ctypes.c_int),
        ("cbWndExtra", ctypes.c_int),
        ("hInstance", HINSTANCE),
        ("hIcon", HICON),
        ("hCursor", HCURSOR),
        ("hbrBackground", HBRUSH),
        ("lpszMenuName", wintypes.LPCWSTR),
        ("lpszClassName", wintypes.LPCWSTR),
    ]


class MSG(ctypes.Structure):
    _fields_ = [
        ("hwnd", wintypes.HWND),
        ("message", wintypes.UINT),
        ("wParam", wintypes.WPARAM),
        ("lParam", wintypes.LPARAM),
        ("time", wintypes.DWORD),
        ("pt", wintypes.POINT),
    ]


class BROWSEINFOW(ctypes.Structure):
    _fields_ = [
        ("hwndOwner", wintypes.HWND),
        ("pidlRoot", ctypes.c_void_p),
        ("pszDisplayName", wintypes.LPWSTR),
        ("lpszTitle", wintypes.LPCWSTR),
        ("ulFlags", wintypes.UINT),
        ("lpfn", ctypes.c_void_p),
        ("lParam", wintypes.LPARAM),
        ("iImage", ctypes.c_int),
    ]


WM_DESTROY = 0x0002
WM_COMMAND = 0x0111
WM_CLOSE = 0x0010
WM_SETFONT = 0x0030
WM_SETTEXT = 0x000C
WM_CTLCOLORBTN = 0x0135
WM_CTLCOLOREDIT = 0x0133
WM_CTLCOLORSTATIC = 0x0138
WM_APP_INSTALL = 0x8001
BM_GETCHECK = 0x00F0
BM_SETCHECK = 0x00F1
BST_CHECKED = 1
EM_SETSEL = 0x00B1
EM_REPLACESEL = 0x00C2
SW_SHOW = 5
SW_HIDE = 0
WS_OVERLAPPED = 0x00000000
WS_CAPTION = 0x00C00000
WS_SYSMENU = 0x00080000
WS_MINIMIZEBOX = 0x00020000
WS_VISIBLE = 0x10000000
WS_CHILD = 0x40000000
WS_TABSTOP = 0x00010000
WS_BORDER = 0x00800000
WS_CLIPSIBLINGS = 0x04000000
WS_VSCROLL = 0x00200000
ES_LEFT = 0x0000
ES_AUTOHSCROLL = 0x0080
ES_MULTILINE = 0x0004
ES_AUTOVSCROLL = 0x0040
ES_READONLY = 0x0800
BS_PUSHBUTTON = 0x00000000
BS_AUTOCHECKBOX = 0x00000003
SS_LEFT = 0x00000000
SS_CENTERIMAGE = 0x00000200
DEFAULT_GUI_FONT = 17
COLOR_WINDOW = 5
TRANSPARENT = 1
IDC_ARROW = 32512
IDI_APPLICATION = 32512
BIF_RETURNONLYFSDIRS = 0x0001
BIF_NEWDIALOGSTYLE = 0x0040


ID_BACK = 1001
ID_NEXT = 1002
ID_CANCEL = 1003
ID_BROWSE = 1004
ID_PATH_EDIT = 1005
ID_CHECK_PATH = 1006
ID_CHECK_PATHEXT = 1007
ID_CHECK_ASSOC = 1008
ID_OPEN_FOLDER = 1009
ID_CHECK_AUTO_UPDATE = 1011


def rgb(red, green, blue):
    return red | (green << 8) | (blue << 16)


def configure_winapi():
    if os.name != "nt":
        return

    kernel32.GetModuleHandleW.restype = HINSTANCE
    kernel32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]

    user32.RegisterClassW.restype = wintypes.ATOM
    user32.RegisterClassW.argtypes = [ctypes.POINTER(WNDCLASSW)]
    user32.CreateWindowExW.restype = wintypes.HWND
    user32.CreateWindowExW.argtypes = [
        wintypes.DWORD,
        wintypes.LPCWSTR,
        wintypes.LPCWSTR,
        wintypes.DWORD,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        wintypes.HWND,
        HMENU,
        HINSTANCE,
        ctypes.c_void_p,
    ]
    user32.DefWindowProcW.restype = LRESULT
    user32.DefWindowProcW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.SendMessageW.restype = LRESULT
    user32.PostMessageW.restype = wintypes.BOOL
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    user32.UpdateWindow.argtypes = [wintypes.HWND]
    user32.DestroyWindow.argtypes = [wintypes.HWND]
    user32.EnableWindow.argtypes = [wintypes.HWND, wintypes.BOOL]
    user32.GetWindowTextLengthW.argtypes = [wintypes.HWND]
    user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user32.LoadCursorW.restype = HCURSOR
    user32.LoadIconW.restype = HICON

    gdi32.GetStockObject.restype = HGDIOBJ
    gdi32.CreateFontW.restype = HGDIOBJ
    gdi32.CreateSolidBrush.restype = HBRUSH
    gdi32.SetBkMode.argtypes = [wintypes.HDC, ctypes.c_int]
    gdi32.SetTextColor.argtypes = [wintypes.HDC, wintypes.DWORD]


configure_winapi()


def windows_command_line_args():
    if os.name != "nt" or not getattr(sys, "frozen", False):
        return sys.argv[1:]

    argc = ctypes.c_int()
    kernel32.GetCommandLineW.restype = wintypes.LPWSTR
    kernel32.LocalFree.argtypes = [wintypes.HLOCAL]
    kernel32.LocalFree.restype = wintypes.HLOCAL
    shell32.CommandLineToArgvW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_int)]
    shell32.CommandLineToArgvW.restype = ctypes.POINTER(wintypes.LPWSTR)
    argv = shell32.CommandLineToArgvW(kernel32.GetCommandLineW(), ctypes.byref(argc))
    if not argv:
        return sys.argv[1:]

    try:
        return [argv[i] for i in range(argc.value)][1:]
    finally:
        kernel32.LocalFree(argv)


class NativeWizard:
    def __init__(self):
        self.hinst = kernel32.GetModuleHandleW(None)
        self.class_name = "BppSetupWindow"
        self.hwnd = None
        self.bg_color = rgb(248, 250, 252)
        self.edit_color = rgb(255, 255, 255)
        self.text_color = rgb(17, 24, 39)
        self.bg_brush = gdi32.CreateSolidBrush(self.bg_color)
        self.edit_brush = gdi32.CreateSolidBrush(self.edit_color)
        self.font = self.create_font(16, 400)
        self.title_font = self.create_font(28, 700)
        self.heading_font = self.create_font(21, 600)
        self.small_font = self.create_font(15, 400)
        self.window_width = 720
        self.window_height = 520
        self.controls = []
        self.page_controls = []
        self.page = 0
        self.install_dir = default_install_dir()
        self.installed_exe = ""
        self.install_succeeded = False
        self._proc = WNDPROC(self.wnd_proc)

    def create_font(self, size, weight):
        return gdi32.CreateFontW(
            -size,
            0,
            0,
            0,
            weight,
            0,
            0,
            0,
            1,
            0,
            0,
            0,
            0,
            "Segoe UI",
        )

    def run(self):
        self.register_class()
        self.create_window()
        user32.ShowWindow(self.hwnd, SW_SHOW)
        user32.UpdateWindow(self.hwnd)

        msg = MSG()
        while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) != 0:
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))

    def register_class(self):
        wc = WNDCLASSW()
        wc.lpfnWndProc = self._proc
        wc.hInstance = self.hinst
        wc.lpszClassName = self.class_name
        wc.hCursor = user32.LoadCursorW(None, IDC_ARROW)
        wc.hIcon = user32.LoadIconW(None, IDI_APPLICATION)
        wc.hbrBackground = self.bg_brush
        atom = user32.RegisterClassW(ctypes.byref(wc))
        if not atom and ctypes.get_last_error() != 1410:
            raise ctypes.WinError(ctypes.get_last_error())

    def create_window(self):
        style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX
        self.hwnd = user32.CreateWindowExW(
            0,
            self.class_name,
            "B++ Setup",
            style,
            -2147483648,
            -2147483648,
            self.window_width,
            self.window_height,
            None,
            None,
            self.hinst,
            None,
        )
        if not self.hwnd:
            raise ctypes.WinError(ctypes.get_last_error())
        self.center_window()
        self.create_shell()
        self.show_page(0)

    def center_window(self):
        width, height = self.window_width, self.window_height
        screen_w = user32.GetSystemMetrics(0)
        screen_h = user32.GetSystemMetrics(1)
        x = max(0, (screen_w - width) // 2)
        y = max(0, (screen_h - height) // 2)
        user32.SetWindowPos(self.hwnd, None, x, y, width, height, 0)

    def create_shell(self):
        self.header_title = self.add_control("STATIC", "B++ Setup", 24, 18, 640, 36, font=self.title_font)
        self.header_subtitle = self.add_control(
            "STATIC",
            f"Install the Windows B++ compiler and .bpp runner  |  version {VERSION}",
            26,
            58,
            640,
            24,
            font=self.small_font,
        )
        self.back_btn = self.add_button("Back", ID_BACK, 424, 442, 82, 30)
        self.next_btn = self.add_button("Install", ID_NEXT, 514, 442, 82, 30)
        self.cancel_btn = self.add_button("Cancel", ID_CANCEL, 604, 442, 82, 30)

    def add_control(self, klass, text, x, y, w, h, control_id=0, style=0, font=None):
        hwnd = user32.CreateWindowExW(
            0,
            klass,
            text,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style,
            x,
            y,
            w,
            h,
            self.hwnd,
            HMENU(control_id),
            self.hinst,
            None,
        )
        user32.SendMessageW(hwnd, WM_SETFONT, wintypes.WPARAM(font or self.font), wintypes.LPARAM(1))
        self.controls.append(hwnd)
        return hwnd

    def add_page_control(self, klass, text, x, y, w, h, control_id=0, style=0, font=None):
        hwnd = self.add_control(klass, text, x, y, w, h, control_id, style, font)
        self.page_controls.append(hwnd)
        return hwnd

    def add_button(self, text, control_id, x, y, w, h):
        return self.add_control("BUTTON", text, x, y, w, h, control_id, WS_TABSTOP | BS_PUSHBUTTON)

    def add_page_button(self, text, control_id, x, y, w, h):
        return self.add_page_control("BUTTON", text, x, y, w, h, control_id, WS_TABSTOP | BS_PUSHBUTTON)

    def clear_page(self):
        for hwnd in self.page_controls:
            user32.DestroyWindow(hwnd)
        self.page_controls = []

    def show_page(self, page):
        self.clear_page()
        self.page = page
        if page == 0:
            self.create_welcome_page()
        elif page == 1:
            self.create_options_page()
        elif page == 2:
            self.create_install_page()
        self.update_nav()

    def create_welcome_page(self):
        self.add_page_control("STATIC", "Welcome to the B++ installer", 32, 112, 640, 30, font=self.heading_font)
        self.add_page_control(
            "STATIC",
            "This wizard installs the Windows build of B++ as a command-line language.",
            32,
            158,
            640,
            24,
        )
        self.add_page_control(
            "STATIC",
            "After setup, open a new terminal and run bpp, b++, or .bpp files.",
            32,
            188,
            640,
            24,
        )
        self.add_page_control(
            "STATIC",
            "Linux is supported separately with: python3 b++.py --install",
            32,
            218,
            640,
            24,
        )
        self.add_page_control(
            "STATIC",
            "Setup installs for the current Windows user and does not require admin rights.",
            32,
            266,
            640,
            24,
        )

    def create_options_page(self):
        self.add_page_control("STATIC", "Choose install options", 32, 104, 640, 30, font=self.heading_font)
        self.add_page_control("STATIC", "Windows install location", 32, 146, 180, 22)
        self.path_edit = self.add_page_control(
            "EDIT",
            self.install_dir,
            32,
            174,
            520,
            24,
            ID_PATH_EDIT,
            WS_BORDER | ES_LEFT | ES_AUTOHSCROLL,
        )
        self.add_page_button("Browse", ID_BROWSE, 568, 172, 96, 30)
        self.path_check = self.add_page_control(
            "BUTTON",
            "",
            32,
            218,
            18,
            18,
            ID_CHECK_PATH,
            WS_TABSTOP | BS_AUTOCHECKBOX,
        )
        self.add_page_control("STATIC", "Add B++ to PATH", 58, 216, 580, 24)
        self.pathext_check = self.add_page_control(
            "BUTTON",
            "",
            32,
            248,
            18,
            18,
            ID_CHECK_PATHEXT,
            WS_TABSTOP | BS_AUTOCHECKBOX,
        )
        self.add_page_control("STATIC", "Allow .bpp files to run from cmd.exe", 58, 246, 580, 24)
        self.assoc_check = self.add_page_control(
            "BUTTON",
            "",
            32,
            278,
            18,
            18,
            ID_CHECK_ASSOC,
            WS_TABSTOP | BS_AUTOCHECKBOX,
        )
        self.add_page_control("STATIC", "Register .bpp files with Windows", 58, 276, 580, 24)
        self.add_page_control("STATIC", f"Updates source: GitHub Releases ({UPDATE_REPO})", 32, 318, 620, 22)
        self.auto_update_check = self.add_page_control(
            "BUTTON",
            "",
            32,
            352,
            18,
            18,
            ID_CHECK_AUTO_UPDATE,
            WS_TABSTOP | BS_AUTOCHECKBOX,
        )
        self.add_page_control("STATIC", "Automatically check GitHub for B++ updates", 58, 350, 580, 24)
        self.set_checked(self.path_check, True)
        self.set_checked(self.pathext_check, True)
        self.set_checked(self.assoc_check, True)
        self.set_checked(self.auto_update_check, True)

    def create_install_page(self):
        if self.install_succeeded:
            self.add_page_control("STATIC", "B++ is installed", 32, 110, 640, 30, font=self.heading_font)
            self.add_page_control("STATIC", f"Installed compiler: {self.installed_exe}", 32, 154, 640, 24)
            self.add_page_control("STATIC", "Open a new terminal and run: bpp --version", 32, 188, 640, 24)
            self.add_page_button("Open install folder", ID_OPEN_FOLDER, 32, 232, 168, 30)
        else:
            self.add_page_control("STATIC", "Installing B++", 32, 104, 640, 30, font=self.heading_font)
            self.log_edit = self.add_page_control(
                "EDIT",
                "",
                32,
                146,
                632,
                230,
                0,
                WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            )
            user32.PostMessageW(self.hwnd, WM_APP_INSTALL, 0, 0)

    def update_nav(self):
        self.set_enabled(self.back_btn, self.page == 1)
        self.set_enabled(self.cancel_btn, not self.install_succeeded)
        if self.page == 0:
            self.set_text(self.next_btn, "Next")
            self.set_enabled(self.next_btn, True)
        elif self.page == 1:
            self.set_text(self.next_btn, "Install")
            self.set_enabled(self.next_btn, True)
        elif self.install_succeeded:
            self.set_text(self.next_btn, "Finish")
            self.set_enabled(self.next_btn, True)
        else:
            self.set_text(self.next_btn, "Installing")
            self.set_enabled(self.next_btn, False)

    def handle_next(self):
        if self.page == 0:
            self.show_page(1)
        elif self.page == 1:
            self.install_dir = self.get_text(self.path_edit).strip() or default_install_dir()
            self.show_page(2)
        else:
            user32.DestroyWindow(self.hwnd)

    def handle_back(self):
        if self.page == 1:
            self.install_dir = self.get_text(self.path_edit).strip() or default_install_dir()
            self.show_page(0)

    def run_install(self):
        self.append_log("Starting B++ setup...")
        try:
            self.installed_exe = install_bpp(
                self.install_dir,
                add_path=self.is_checked(self.path_check),
                add_pathext=self.is_checked(self.pathext_check),
                associate=self.is_checked(self.assoc_check),
                auto_update=self.is_checked(self.auto_update_check),
                log=self.append_log,
            )
            self.append_log("Done.")
            self.install_succeeded = True
            self.show_page(2)
        except Exception as exc:
            self.append_log("Install failed: " + str(exc))
            self.show_error("B++ Setup", str(exc))
            self.set_text(self.next_btn, "Install")
            self.set_enabled(self.next_btn, True)
            self.set_enabled(self.cancel_btn, True)

    def append_log(self, message):
        if not hasattr(self, "log_edit"):
            return
        text = message + "\r\n"
        user32.SendMessageW(self.log_edit, EM_SETSEL, wintypes.WPARAM(-1), wintypes.LPARAM(-1))
        user32.SendMessageW(self.log_edit, EM_REPLACESEL, False, text)

    def browse_folder(self):
        buffer = ctypes.create_unicode_buffer(260)
        info = BROWSEINFOW()
        info.hwndOwner = self.hwnd
        info.pszDisplayName = buffer
        info.lpszTitle = "Choose where to install B++"
        info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE
        pidl = shell32.SHBrowseForFolderW(ctypes.byref(info))
        if pidl:
            path = ctypes.create_unicode_buffer(260)
            if shell32.SHGetPathFromIDListW(pidl, path):
                self.set_text(self.path_edit, path.value)
            ole32.CoTaskMemFree(pidl)

    def open_install_folder(self):
        path = os.path.abspath(os.path.expandvars(self.install_dir))
        if os.path.isdir(path):
            subprocess.Popen(["explorer", path])

    def wnd_proc(self, hwnd, msg, wparam, lparam):
        if msg == WM_COMMAND:
            control_id = int(wparam) & 0xFFFF
            if control_id == ID_NEXT:
                self.handle_next()
                return 0
            if control_id == ID_BACK:
                self.handle_back()
                return 0
            if control_id == ID_CANCEL:
                user32.DestroyWindow(hwnd)
                return 0
            if control_id == ID_BROWSE:
                self.browse_folder()
                return 0
            if control_id == ID_OPEN_FOLDER:
                self.open_install_folder()
                return 0
        elif msg == WM_APP_INSTALL:
            self.run_install()
            return 0
        elif msg in (WM_CTLCOLORSTATIC, WM_CTLCOLORBTN):
            hdc = wintypes.HDC(wparam)
            gdi32.SetBkMode(hdc, TRANSPARENT)
            gdi32.SetTextColor(hdc, self.text_color)
            return self.bg_brush
        elif msg == WM_CTLCOLOREDIT:
            hdc = wintypes.HDC(wparam)
            gdi32.SetTextColor(hdc, self.text_color)
            return self.edit_brush
        elif msg in (WM_CLOSE, WM_DESTROY):
            user32.PostQuitMessage(0)
            return 0
        return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

    def set_text(self, hwnd, text):
        user32.SendMessageW(hwnd, WM_SETTEXT, wintypes.WPARAM(0), text)

    def get_text(self, hwnd):
        length = user32.GetWindowTextLengthW(hwnd)
        buffer = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buffer, length + 1)
        return buffer.value

    def set_enabled(self, hwnd, enabled):
        user32.EnableWindow(hwnd, bool(enabled))

    def set_checked(self, hwnd, checked):
        user32.SendMessageW(hwnd, BM_SETCHECK, wintypes.WPARAM(BST_CHECKED if checked else 0), wintypes.LPARAM(0))

    def is_checked(self, hwnd):
        return user32.SendMessageW(hwnd, BM_GETCHECK, wintypes.WPARAM(0), wintypes.LPARAM(0)) == BST_CHECKED

    def show_error(self, title, message):
        user32.MessageBoxW(self.hwnd, message, title, 0x10)


def main():
    if os.name != "nt":
        print("This installer is for Windows.", file=sys.stderr)
        return 1

    args = windows_command_line_args()
    if "--silent" in args:
        install_dir = default_install_dir()
        add_path = "--no-path" not in args
        add_pathext = "--no-pathext" not in args
        associate = "--no-associate" not in args
        auto_update = "--no-auto" not in args
        if "--install-dir" in args:
            index = args.index("--install-dir")
            if index + 1 >= len(args):
                print("--install-dir needs a path", file=sys.stderr)
                return 2
            install_dir = args[index + 1]
        install_bpp(
            install_dir,
            add_path=add_path,
            add_pathext=add_pathext,
            associate=associate,
            auto_update=auto_update,
            log=print,
        )
        return 0

    NativeWizard().run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
