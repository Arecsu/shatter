// ShatterSetup.exe: installs, updates or removes Shatter's PhysXLoader.dll in a Mirror's Edge install.
//
// The game is found only from the user: a path argument (also what a drag of MirrorsEdge.exe or its folder onto this
// exe gives), else a welcome screen and then the native file dialog. No registry, no guessed install locations, so it works the same inside
// Wine/Proton prefixes (Heroic, Lutris, protontricks-launch).
//
// <Binaries>/PhysXLoader.dll, by its version resource:
//   original (PhysXLoader 2.8.0.7): install. The original is copied to PhysXLoader.dll.original first unless that file
//     already exists (an existing backup is never overwritten).
//   Shatter (ProductName "Shatter"): update to this version, or uninstall (PhysXLoader.dll.original back in place,
//     only if it is the original; otherwise the player is pointed to Steam Verify / GOG Repair).
//   anything else: nothing is changed.
// Some copies (EA app) have no <Binaries>/PhysXLoader.dll, only <Binaries>/PhysXLocal/PhysXLoader.dll: the game loads
// "PhysXLoader.dll" by name and falls back to the PhysXLocal one only when that fails. If the PhysXLocal one is the
// original, Shatter's DLL is added to Binaries (so it wins), with the marker PhysXLoader.dll.shatter-added instead of
// a backup; uninstalling deletes both. PhysXLocal is never changed.
// Shatter's DLL is embedded in this exe (resource "PHYSXLOADER", RCDATA). It is written next to the target as
// PhysXLoader.dll.shatter-new and then moved over PhysXLoader.dll, so a failed write leaves the game as it was.
//
// Game settings (a button after installing, and on the "already installed" screen): the frame rate cap and the intro
// movie, in the player's Documents\EA Games\Mirror's Edge\TdGame\Config\TdEngine.ini. Only those keys are touched,
// whatever way the player wrote them before (spacing, case, duplicates, line endings, encoding); the file is copied
// once to TdEngine.ini.shatter-backup before the first change, and uninstalling offers to put the old values back.
//
// Scripted use: ShatterSetup.exe <MirrorsEdge.exe or its folder> /install|/uninstall [/quiet]
//   [/fps=N|default] [/skipintro|/intro]. /install installs or updates without asking; /uninstall restores
// the backup. /fps and /skipintro|/intro change the game settings after installing or updating. /quiet shows no
// dialogs: the result goes to stderr and the exit code (0 done, 1 nothing changed or failed).

#include <windows.h>

#include <commctrl.h>
#include <commdlg.h>
#include <math.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include <string>
#include <vector>

#include "../shim/version.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "user32.lib")

#define W2(x) L##x
#define W(x) W2(x)

namespace {

const wchar_t kTitle[] = L"Shatter " W(SHATTER_VERSION_STR) L" setup";
const wchar_t kVersion[] = W(SHATTER_VERSION_STR);

bool g_quiet = false;
enum class Action { Ask, Install, Uninstall } g_action = Action::Ask;

// Game settings asked for on the command line (/fps=, /skipintro, /intro).
enum class FpsFlag { None, Default, Value } g_fpsFlag = FpsFlag::None;
double g_fpsValue = 0;
int g_introFlag = -1;  // -1 unchanged, 0 skip, 1 play

enum class Kind { Missing, Original, Shatter, Unknown };

struct LoaderInfo {
  Kind kind = Kind::Missing;
  wchar_t version[64] = L"";  // "2.8.0.7" or Shatter's FileVersion
};

LoaderInfo inspect(const wchar_t* path) {
  LoaderInfo info;
  if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return info;
  info.kind = Kind::Unknown;
  DWORD h = 0;
  const DWORD n = GetFileVersionInfoSizeW(path, &h);
  if (!n) return info;
  BYTE* buf = new BYTE[n];
  if (GetFileVersionInfoW(path, 0, n, buf)) {
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (VerQueryValueW(buf, L"\\", reinterpret_cast<void**>(&fi), &len) && fi) {
      swprintf(info.version, 64, L"%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
               HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
      if (fi->dwFileVersionMS == 0x00020008 && fi->dwFileVersionLS == 0x00000007) info.kind = Kind::Original;
    }
    // Ours carries ProductName "Shatter" (shim/version.rc), language 0409 / codepage 04b0.
    wchar_t* s = nullptr;
    if (VerQueryValueW(buf, L"\\StringFileInfo\\040904b0\\ProductName", reinterpret_cast<void**>(&s), &len) && s &&
        !wcscmp(s, L"Shatter")) {
      info.kind = Kind::Shatter;
      if (VerQueryValueW(buf, L"\\StringFileInfo\\040904b0\\FileVersion", reinterpret_cast<void**>(&s), &len) && s)
        swprintf(info.version, 64, L"%ls", s);
    }
  }
  delete[] buf;
  return info;
}

// ---- dialogs -------------------------------------------------------------------------------

// Every dialog centred on the work area of the monitor under the cursor. With no owner window the placement is the
// system's: Wine centres the process's first window and puts the later ones at the top left.
void centerOnMonitor(HWND h) {
  POINT pt;
  GetCursorPos(&pt);
  MONITORINFO mi = {sizeof mi};
  RECT r;
  if (!GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi) || !GetWindowRect(h, &r)) return;
  const RECT& a = mi.rcWork;
  const int w = r.right - r.left, ht = r.bottom - r.top;
  SetWindowPos(h, nullptr, a.left + (a.right - a.left - w) / 2, a.top + (a.bottom - a.top - ht) / 2, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

int taskDialog(const wchar_t* main, const wchar_t* content, PCWSTR icon, const TASKDIALOG_BUTTON* buttons, UINT nButtons,
               TASKDIALOG_COMMON_BUTTON_FLAGS common, bool commandLinks = false) {
  if (g_quiet) {
    fwprintf(stderr, L"%ls: %ls\n", main, content ? content : L"");
    return nButtons ? buttons[0].nButtonID : IDOK;
  }
  TASKDIALOGCONFIG c = {};
  c.cbSize = sizeof c;
  c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW |
              (commandLinks ? TDF_USE_COMMAND_LINKS : 0);
  c.pszWindowTitle = kTitle;
  c.pszMainIcon = icon;
  c.pszMainInstruction = main;
  c.pszContent = content;
  c.pButtons = buttons;
  c.cButtons = nButtons;
  c.dwCommonButtons = common;
  c.pfCallback = [](HWND h, UINT msg, WPARAM, LPARAM, LONG_PTR) -> HRESULT {
    if (msg == TDN_CREATED) centerOnMonitor(h);
    return S_OK;
  };
  int pressed = IDCANCEL;
  if (FAILED(TaskDialogIndirect(&c, &pressed, nullptr, nullptr))) {
    // No TaskDialog (comctl32 v6 missing): plain message box with the same text.
    wchar_t text[2048];
    swprintf(text, 2048, L"%ls\n\n%ls", main, content ? content : L"");
    const UINT type = nButtons ? MB_OKCANCEL : MB_OK;
    pressed = MessageBoxW(nullptr, text, kTitle, type | MB_ICONINFORMATION) == IDOK && nButtons ? buttons[0].nButtonID
                                                                                                 : IDCANCEL;
  }
  return pressed;
}

void info(const wchar_t* main, const wchar_t* content) {
  taskDialog(main, content, TD_INFORMATION_ICON, nullptr, 0, TDCBF_OK_BUTTON);
}
void error(const wchar_t* main, const wchar_t* content) {
  taskDialog(main, content, TD_ERROR_ICON, nullptr, 0, TDCBF_OK_BUTTON);
}

// Explains a failed file operation: the game still running, or a folder that needs administrator rights.
bool fileError(const wchar_t* what, DWORD gle) {
  wchar_t sys[512] = L"";
  FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, gle, 0, sys, 512, nullptr);
  const wchar_t* hint = gle == ERROR_SHARING_VIOLATION || gle == ERROR_LOCK_VIOLATION
                            ? L"The file is in use. Close the game and try again.\n\n"
                        : gle == ERROR_ACCESS_DENIED ? L"Access denied. Run ShatterSetup.exe as administrator.\n\n"
                                                     : L"";
  wchar_t text[1536];
  swprintf(text, 1536, L"%ls\n%ls(error %lu) %ls", what, hint, (unsigned long)gle, sys);
  error(L"Nothing could be changed", text);
  return false;
}

bool pickExe(wchar_t* path, DWORD size) {
  OPENFILENAMEW o = {};
  o.lStructSize = sizeof o;
  o.lpstrFilter = L"MirrorsEdge.exe\0MirrorsEdge.exe\0Programs (*.exe)\0*.exe\0";
  o.lpstrFile = path;
  o.nMaxFile = size;
  o.lpstrTitle = L"Select MirrorsEdge.exe (in the game's Binaries folder)";
  o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_EXPLORER |
            OFN_ENABLEHOOK | OFN_ENABLESIZING;
  // Centred as the other dialogs once it is laid out (the hook's dialog is a child of the real one).
  o.lpfnHook = [](HWND h, UINT msg, WPARAM, LPARAM lp) -> UINT_PTR {
    if (msg == WM_NOTIFY && reinterpret_cast<const OFNOTIFYW*>(lp)->hdr.code == CDN_INITDONE) centerOnMonitor(GetParent(h));
    return 0;
  };
  path[0] = 0;
  return GetOpenFileNameW(&o) != 0;
}

// ---- file steps ----------------------------------------------------------------------------

// Writes the embedded DLL over `target` (via a temporary file next to it). Returns 0 or the Win32 error.
DWORD writeOurs(const wchar_t* target) {
  HRSRC r = FindResourceW(nullptr, L"PHYSXLOADER", MAKEINTRESOURCEW(10));  // RT_RCDATA
  HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
  const void* data = g ? LockResource(g) : nullptr;
  const DWORD size = r ? SizeofResource(nullptr, r) : 0;
  if (!data || !size) return ERROR_RESOURCE_DATA_NOT_FOUND;
  wchar_t tmp[MAX_PATH * 2];
  swprintf(tmp, MAX_PATH * 2, L"%ls.shatter-new", target);
  HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return GetLastError();
  DWORD written = 0;
  const BOOL ok = WriteFile(f, data, size, &written, nullptr) && written == size && FlushFileBuffers(f);
  const DWORD gle = ok ? 0 : GetLastError();
  CloseHandle(f);
  if (!ok) {
    DeleteFileW(tmp);
    return gle ? gle : ERROR_WRITE_FAULT;
  }
  if (!MoveFileExW(tmp, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const DWORD e = GetLastError();
    DeleteFileW(tmp);
    return e;
  }
  return 0;
}

// ---- TdEngine.ini --------------------------------------------------------------------------
//
// The file as lines, read in whatever encoding it has (UTF-16 with BOM, UTF-8 with or without BOM, else the ANSI
// code page) and written back the same way, with its own line endings. Keys and section names match without case
// and with any spacing around them and the "="; a leading "+" (array add) is accepted, ";" and "#" lines are comments.

struct Ini {
  std::vector<std::wstring> lines;
  enum Enc { Ansi, Utf8, Utf8Bom, Utf16 } enc = Utf8;
  bool crlf = true, finalEol = true;
};

const wchar_t kGameEngine[] = L"Engine.GameEngine";
const wchar_t kMovies[] = L"FullScreenMovie";
const double kDefaultFps = 62, kDefaultMinFps = 22;  // the engine's own values (BaseEngine.ini)

std::wstring trim(const std::wstring& s) {
  size_t a = 0, b = s.size();
  while (a < b && iswspace(s[a])) ++a;
  while (b > a && iswspace(s[b - 1])) --b;
  return s.substr(a, b - a);
}

bool loadIni(const wchar_t* path, Ini& ini) {
  HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  const DWORD n = GetFileSize(f, nullptr);
  std::string b(n, '\0');
  DWORD got = 0;
  const bool ok = n == INVALID_FILE_SIZE ? false : (!n || (ReadFile(f, &b[0], n, &got, nullptr) && got == n));
  CloseHandle(f);
  if (!ok) return false;
  std::wstring text;
  if (n >= 2 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xFE) {
    ini.enc = Ini::Utf16;
    text.assign(reinterpret_cast<const wchar_t*>(b.data() + 2), (n - 2) / 2);
  } else {
    size_t off = 0;
    ini.enc = Ini::Utf8;
    if (n >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF) {
      ini.enc = Ini::Utf8Bom;
      off = 3;
    }
    const char* p = b.data() + off;
    const int len = (int)(n - off);
    UINT cp = CP_UTF8;
    int m = len ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, len, nullptr, 0) : 0;
    if (len && !m) {  // not UTF-8: the ANSI code page
      cp = CP_ACP;
      ini.enc = Ini::Ansi;
      m = MultiByteToWideChar(CP_ACP, 0, p, len, nullptr, 0);
    }
    text.resize(m);
    if (m) MultiByteToWideChar(cp, 0, p, len, &text[0], m);
  }
  size_t crlf = 0, lf = 0, start = 0;
  ini.lines.clear();
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != L'\n') continue;
    size_t end = i;
    if (end > start && text[end - 1] == L'\r') --end, ++crlf;
    else ++lf;
    ini.lines.push_back(text.substr(start, end - start));
    start = i + 1;
  }
  ini.finalEol = start == text.size();
  if (!ini.finalEol) ini.lines.push_back(text.substr(start));
  ini.crlf = crlf >= lf;
  return true;
}

// Writes next to the file and moves it over, keeping the read-only attribute some players set so the game
// does not reset the file. Returns 0 or the Win32 error.
DWORD saveIni(const wchar_t* path, const Ini& ini) {
  std::wstring text;
  for (size_t i = 0; i < ini.lines.size(); ++i) {
    text += ini.lines[i];
    if (i + 1 < ini.lines.size() || ini.finalEol) text += ini.crlf ? L"\r\n" : L"\n";
  }
  std::string out;
  if (ini.enc == Ini::Utf16) {
    out = "\xFF\xFE";
    out.append(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t));
  } else {
    if (ini.enc == Ini::Utf8Bom) out = "\xEF\xBB\xBF";
    const UINT cp = ini.enc == Ini::Ansi ? CP_ACP : CP_UTF8;
    const int m = WideCharToMultiByte(cp, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    const size_t at = out.size();
    out.resize(at + m);
    if (m) WideCharToMultiByte(cp, 0, text.data(), (int)text.size(), &out[at], m, nullptr, nullptr);
  }
  wchar_t tmp[MAX_PATH * 2];
  swprintf(tmp, MAX_PATH * 2, L"%ls.shatter-new", path);
  HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return GetLastError();
  DWORD written = 0;
  const BOOL ok = WriteFile(f, out.data(), (DWORD)out.size(), &written, nullptr) && written == out.size() &&
                  FlushFileBuffers(f);
  DWORD gle = ok ? 0 : GetLastError();
  CloseHandle(f);
  const DWORD attrs = GetFileAttributesW(path);
  const bool readOnly = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY);
  if (ok && readOnly) SetFileAttributesW(path, attrs & ~FILE_ATTRIBUTE_READONLY);
  if (ok && !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) gle = GetLastError();
  if (gle) DeleteFileW(tmp);
  if (readOnly) SetFileAttributesW(path, attrs);
  return ok ? gle : (gle ? gle : ERROR_WRITE_FAULT);
}

// The section name if `line` is a header ("[Engine.GameEngine]", spaces allowed inside and around).
bool isHeader(const std::wstring& line, std::wstring* name) {
  const std::wstring t = trim(line);
  if (t.size() < 2 || t.front() != L'[' || t.back() != L']') return false;
  *name = trim(t.substr(1, t.size() - 2));
  return true;
}

// True if `line` sets `key`; its value is trimmed and loses surrounding quotes.
bool isKey(const std::wstring& line, const wchar_t* key, std::wstring* value) {
  std::wstring t = trim(line);
  if (t.empty() || t[0] == L';' || t[0] == L'#') return false;
  if (t[0] == L'+') t.erase(0, 1);
  const size_t eq = t.find(L'=');
  if (eq == std::wstring::npos || _wcsicmp(trim(t.substr(0, eq)).c_str(), key)) return false;
  std::wstring v = trim(t.substr(eq + 1));
  if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"') v = v.substr(1, v.size() - 2);
  *value = v;
  return true;
}

// Line indexes of every `key` in every copy of `section` (a section can appear more than once), with their values.
std::vector<size_t> findKey(const Ini& ini, const wchar_t* section, const wchar_t* key,
                            std::vector<std::wstring>* values = nullptr) {
  std::vector<size_t> at;
  bool in = false;
  std::wstring name, v;
  for (size_t i = 0; i < ini.lines.size(); ++i) {
    if (isHeader(ini.lines[i], &name)) in = !_wcsicmp(name.c_str(), section);
    else if (in && isKey(ini.lines[i], key, &v)) {
      at.push_back(i);
      if (values) values->push_back(v);
    }
  }
  return at;
}

bool getKey(const Ini& ini, const wchar_t* section, const wchar_t* key, std::wstring* value) {
  std::vector<std::wstring> v;
  if (findKey(ini, section, key, &v).empty()) return false;
  *value = v[0];
  return true;
}

// Replaces every line of `key` in `section` by one line per value (none: the key is removed), at the first line
// the key had, else right after the section's first header; a missing section is added at the end.
void setKey(Ini& ini, const wchar_t* section, const wchar_t* key, const std::vector<std::wstring>& values) {
  const std::vector<size_t> at = findKey(ini, section, key);
  size_t pos = at.empty() ? ini.lines.size() : at[0];
  for (size_t i = at.size(); i-- > 0;) ini.lines.erase(ini.lines.begin() + at[i]);
  if (values.empty()) return;
  if (at.empty()) {
    std::wstring name;
    bool found = false;
    for (size_t i = 0; i < ini.lines.size() && !found; ++i)
      if (isHeader(ini.lines[i], &name) && !_wcsicmp(name.c_str(), section)) pos = i + 1, found = true;
    if (!found) {
      if (!ini.lines.empty() && !trim(ini.lines.back()).empty()) ini.lines.push_back(L"");
      ini.lines.push_back(L"[" + std::wstring(section) + L"]");
      pos = ini.lines.size();
      ini.finalEol = true;
    }
  }
  for (size_t i = 0; i < values.size(); ++i)
    ini.lines.insert(ini.lines.begin() + pos + i, std::wstring(key) + L"=" + values[i]);
}
void setKey(Ini& ini, const wchar_t* section, const wchar_t* key, const std::wstring& value) {
  setKey(ini, section, key, std::vector<std::wstring>{value});
}

bool parseNumber(const std::wstring& s, double* out) {  // "62", "62.0", "62.000000", "62f"
  wchar_t* end = nullptr;
  const double v = wcstod(s.c_str(), &end);
  if (end == s.c_str() || !(v > 0)) return false;
  *out = v;
  return true;
}

std::wstring formatFps(double v) {
  wchar_t s[32];
  if (fabs(v - floor(v + 0.5)) < 1e-6) swprintf(s, 32, L"%d", (int)floor(v + 0.5));
  else swprintf(s, 32, L"%g", v);
  return s;
}

// What the game reads from the file, with the engine's defaults for missing or unreadable values.
struct GameSettings {
  bool smooth = true;
  double maxFps = kDefaultFps, minFps = kDefaultMinFps;
  std::vector<std::wstring> movies;  // StartupMovies entries; none means the intro is skipped
  bool introPlays() const { return !movies.empty(); }
};

GameSettings readSettings(const Ini& ini) {
  GameSettings s;
  std::wstring v;
  if (getKey(ini, kGameEngine, L"bSmoothFrameRate", &v))
    s.smooth = !_wcsicmp(v.c_str(), L"true") || !_wcsicmp(v.c_str(), L"yes") || !_wcsicmp(v.c_str(), L"on") ||
               _wtoi(v.c_str()) != 0;
  if (getKey(ini, kGameEngine, L"MaxSmoothedFrameRate", &v)) parseNumber(v, &s.maxFps);
  if (getKey(ini, kGameEngine, L"MinSmoothedFrameRate", &v)) parseNumber(v, &s.minFps);
  std::vector<std::wstring> movies;
  findKey(ini, kMovies, L"StartupMovies", &movies);
  for (const std::wstring& m : movies)
    if (!m.empty()) s.movies.push_back(m);
  return s;
}

std::wstring describe(const GameSettings& s) {
  std::wstring d = s.smooth ? L"cap " + formatFps(s.maxFps) + L" fps" : L"no cap (frame rate smoothing off)";
  if (s.smooth && fabs(s.minFps - kDefaultMinFps) > 1e-6) d += L" (minimum " + formatFps(s.minFps) + L")";
  return d + (s.introPlays() ? L", intro movie plays" : L", intro movie skipped");
}

// A cap keeps smoothing on (turning it off is reported to cause bugs); a minimum above the new cap goes back to 22.
void applyFps(Ini& ini, double fps) {
  const GameSettings s = readSettings(ini);
  if (!s.smooth) setKey(ini, kGameEngine, L"bSmoothFrameRate", L"TRUE");
  if (fabs(s.maxFps - fps) > 1e-6 || findKey(ini, kGameEngine, L"MaxSmoothedFrameRate").size() != 1)
    setKey(ini, kGameEngine, L"MaxSmoothedFrameRate", formatFps(fps));
  if (s.minFps > fps) setKey(ini, kGameEngine, L"MinSmoothedFrameRate", formatFps(kDefaultMinFps));
}

void applyIntro(Ini& ini, bool play) {
  if (play == readSettings(ini).introPlays()) return;
  setKey(ini, kMovies, L"StartupMovies", play ? std::vector<std::wstring>{L"StartupMovie"} : std::vector<std::wstring>{});
}

bool settingsPath(wchar_t* ini, DWORD size) {
  PWSTR docs = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) return false;
  swprintf(ini, size, L"%ls\\EA Games\\Mirror's Edge\\TdGame\\Config\\TdEngine.ini", docs);
  CoTaskMemFree(docs);
  return true;
}

// Copies the file once before Shatter's first change; an existing backup is never overwritten.
void backupIni(const wchar_t* path) {
  wchar_t b[MAX_PATH * 2];
  swprintf(b, MAX_PATH * 2, L"%ls.shatter-backup", path);
  CopyFileW(path, b, TRUE);
}

// Saves `ini` only if a line changed (backing up the file first). Returns 0 or the Win32 error.
DWORD saveIfChanged(const wchar_t* path, const Ini& before, const Ini& after) {
  if (before.lines == after.lines && before.finalEol == after.finalEol) return 0;
  backupIni(path);
  return saveIni(path, after);
}

// The slide steering fix lives in shatter.ini next to the DLL (Binaries), key slide_steering_fix in [shatter]; off
// unless the key says on. It only works on the exe build its addresses come from (shim/game_hooks.cpp).
const wchar_t kShatterSection[] = L"shatter";

bool knownGameExe(const wchar_t* dir) {
  wchar_t exe[MAX_PATH * 2];
  swprintf(exe, MAX_PATH * 2, L"%ls\\MirrorsEdge.exe", dir);
  HANDLE f = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  IMAGE_DOS_HEADER dos = {};
  IMAGE_NT_HEADERS32 nt = {};
  DWORD got = 0;
  bool ok = ReadFile(f, &dos, sizeof dos, &got, nullptr) && got == sizeof dos && dos.e_magic == IMAGE_DOS_SIGNATURE &&
            SetFilePointer(f, dos.e_lfanew, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
            ReadFile(f, &nt, sizeof nt, &got, nullptr) && got == sizeof nt && nt.Signature == IMAGE_NT_SIGNATURE;
  CloseHandle(f);
  return ok && nt.FileHeader.TimeDateStamp == SHATTER_GAME_EXE_TIMESTAMP;
}

void shatterIniPath(const wchar_t* dir, wchar_t* path) { swprintf(path, MAX_PATH * 2, L"%ls\\shatter.ini", dir); }

bool slideFixOn(const wchar_t* dir) {
  wchar_t path[MAX_PATH * 2];
  shatterIniPath(dir, path);
  Ini ini;
  std::wstring v;
  if (!loadIni(path, ini) || !getKey(ini, kShatterSection, L"slide_steering_fix", &v)) return false;
  v = trim(v.substr(0, v.find_first_of(L";#")));
  return !_wcsicmp(v.c_str(), L"on") || !_wcsicmp(v.c_str(), L"true") || !_wcsicmp(v.c_str(), L"yes") || v == L"1";
}

// Sets the key in shatter.ini, creating the file if needed. Returns 0 or the Win32 error.
DWORD setSlideFix(const wchar_t* dir, bool on) {
  wchar_t path[MAX_PATH * 2];
  shatterIniPath(dir, path);
  Ini ini;
  if (!loadIni(path, ini)) ini = Ini{};
  setKey(ini, kShatterSection, L"slide_steering_fix", on ? L"on" : L"off");
  return saveIni(path, ini);
}

// The game settings dialog. In: the file as read. Out: what to change.
struct SettingsDialog {
  const wchar_t* path;
  GameSettings now;
  bool movieFileMissing;
  int initialRadio;  // control id, or 0 when no radio fits (smoothing off)
  int initialValue;
  bool slideFixAvailable = false, slideFixNow = false;
  bool changeFps = false, changeIntro = false, introPlays = true, changeSlideFix = false, slideFix = false;
  double fps = 0;
};

INT_PTR CALLBACK settingsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  SettingsDialog* d = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(h, DWLP_USER));
  switch (msg) {
    case WM_INITDIALOG: {
      d = reinterpret_cast<SettingsDialog*>(lp);
      SetWindowLongPtrW(h, DWLP_USER, lp);
      SetWindowTextW(h, kTitle);
      centerOnMonitor(h);
      SetDlgItemTextW(h, IDC_FPS_NOTE,
                      L"Above 62, Shatter keeps cloth and debris behaving the same. The game itself loses most "
                      L"left/right steering on steep slides (like the building slide in chapter 1); the option below "
                      L"fixes that. Some speedrun tricks depend on the 62 cap. With \"VSync\" on in the game's video "
                      L"options, your monitor's refresh rate is still the limit. Frame rate smoothing stays on: "
                      L"turning it off is reported to cause bugs.");
      if (d->slideFixAvailable) {
        SetDlgItemTextW(h, IDC_SLIDEFIX, L"Fix slide steering above 62 fps (changes player movement)");
        CheckDlgButton(h, IDC_SLIDEFIX, d->slideFixNow ? BST_CHECKED : BST_UNCHECKED);
      } else {
        SetDlgItemTextW(h, IDC_SLIDEFIX, L"Fix slide steering above 62 fps (not available for this game version)");
        EnableWindow(GetDlgItem(h, IDC_SLIDEFIX), FALSE);
      }
      SetDlgItemInt(h, IDC_FPS_VALUE, d->initialValue, FALSE);
      SendDlgItemMessageW(h, IDC_FPS_VALUE, EM_LIMITTEXT, 4, 0);
      if (d->initialRadio) CheckDlgButton(h, d->initialRadio, BST_CHECKED);
      if (d->movieFileMissing) {
        SetDlgItemTextW(h, IDC_INTRO, L"Skip the intro movie (already skipped: StartupMovie.bik is not in the game)");
        CheckDlgButton(h, IDC_INTRO, BST_CHECKED);
        EnableWindow(GetDlgItem(h, IDC_INTRO), FALSE);
      } else {
        SetDlgItemTextW(h, IDC_INTRO, L"Skip the intro movie at startup");
        CheckDlgButton(h, IDC_INTRO, d->now.introPlays() ? BST_UNCHECKED : BST_CHECKED);
      }
      SetDlgItemTextW(h, IDC_CURRENT, (L"Now in the file: " + describe(d->now) + L".").c_str());
      SetDlgItemTextW(h, IDC_PATH, d->path);
      return TRUE;
    }
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case IDC_FPS_VALUE:  // typing a number picks "Custom"
          if (HIWORD(wp) == EN_CHANGE && GetFocus() == GetDlgItem(h, IDC_FPS_VALUE)) {
            CheckDlgButton(h, IDC_FPS_DEFAULT, BST_UNCHECKED);
            CheckDlgButton(h, IDC_FPS_CUSTOM, BST_CHECKED);
          }
          return TRUE;
        case IDOK: {
          const int radio = IsDlgButtonChecked(h, IDC_FPS_DEFAULT)  ? IDC_FPS_DEFAULT
                            : IsDlgButtonChecked(h, IDC_FPS_CUSTOM) ? IDC_FPS_CUSTOM
                                                                     : 0;
          BOOL ok = FALSE;
          const int value = (int)GetDlgItemInt(h, IDC_FPS_VALUE, &ok, FALSE);
          if (radio == IDC_FPS_CUSTOM && (!ok || value < 30 || value > 1000)) {
            MessageBoxW(h, L"Enter a frame rate from 30 to 1000.", kTitle, MB_OK | MB_ICONWARNING);
            SetFocus(GetDlgItem(h, IDC_FPS_VALUE));
            SendDlgItemMessageW(h, IDC_FPS_VALUE, EM_SETSEL, 0, -1);
            return TRUE;
          }
          d->changeFps = radio && (radio != d->initialRadio || (radio == IDC_FPS_CUSTOM && value != d->initialValue));
          d->fps = radio == IDC_FPS_DEFAULT ? kDefaultFps : value;
          d->introPlays = IsDlgButtonChecked(h, IDC_INTRO) != BST_CHECKED;
          d->changeIntro = !d->movieFileMissing && d->introPlays != d->now.introPlays();
          d->slideFix = IsDlgButtonChecked(h, IDC_SLIDEFIX) == BST_CHECKED;
          d->changeSlideFix = d->slideFixAvailable && d->slideFix != d->slideFixNow;
          EndDialog(h, IDOK);
          return TRUE;
        }
        case IDCANCEL:
          EndDialog(h, IDCANCEL);
          return TRUE;
      }
      break;
  }
  return FALSE;
}

// Says where the file should be when it is not there (the game makes it on its first start).
void settingsMissing(const wchar_t* path) {
  wchar_t text[1024];
  swprintf(text, 1024,
           L"Not found:\n%ls\n\nStart the game once so it creates its settings, then run this setup again.\n"
           L"On Linux, run the setup in the game's Wine/Proton prefix: its Documents folder holds the file.",
           path);
  error(L"The game's settings file was not found", text);
}

// The "Game settings" button: the dialog, then the change. `dir` is the game's Binaries folder.
void gameSettings(const wchar_t* dir) {
  wchar_t path[MAX_PATH * 2];
  Ini ini;
  if (!settingsPath(path, MAX_PATH * 2) || !loadIni(path, ini)) return settingsMissing(path);
  wchar_t movie[MAX_PATH * 2];
  swprintf(movie, MAX_PATH * 2, L"%ls\\..\\TdGame\\Movies\\StartupMovie.bik", dir);
  SettingsDialog d = {path, readSettings(ini), GetFileAttributesW(movie) == INVALID_FILE_ATTRIBUTES};
  const int rounded = (int)floor(d.now.maxFps + 0.5);
  d.initialValue = rounded;
  d.initialRadio = !d.now.smooth                             ? 0
                   : fabs(d.now.maxFps - kDefaultFps) < 1e-6 ? IDC_FPS_DEFAULT
                                                             : IDC_FPS_CUSTOM;
  if (d.initialRadio != IDC_FPS_CUSTOM) d.initialValue = 144;  // a common high refresh rate, as a starting point
  d.slideFixAvailable = knownGameExe(dir);
  d.slideFixNow = d.slideFixAvailable && slideFixOn(dir);
  if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), nullptr, settingsProc,
                      reinterpret_cast<LPARAM>(&d)) != IDOK ||
      (!d.changeFps && !d.changeIntro && !d.changeSlideFix))
    return;
  Ini after = ini;
  if (d.changeFps) applyFps(after, d.fps);
  if (d.changeIntro) applyIntro(after, d.introPlays);
  if (const DWORD e = saveIfChanged(path, ini, after)) {
    fileError(L"Could not write TdEngine.ini.", e);
    return;
  }
  if (d.changeSlideFix)
    if (const DWORD e = setSlideFix(dir, d.slideFix)) {
      fileError(L"Could not write shatter.ini.", e);
      return;
    }
  const bool fixNow = d.changeSlideFix ? d.slideFix : d.slideFixNow;
  const std::wstring text = L"Now: " + describe(readSettings(after)) +
                            (d.slideFixAvailable ? (fixNow ? L", slide steering fix on" : L", slide steering fix off")
                                                 : L"") +
                            L".\n\nIf the game ever resets them, run this setup again and choose Game settings.";
  info(L"Game settings saved", text.c_str());
}

// /fps= and /skipintro|/intro after installing or updating. Returns false if they could not be applied.
bool applyFlags() {
  if (g_fpsFlag == FpsFlag::None && g_introFlag < 0) return true;
  wchar_t path[MAX_PATH * 2];
  Ini ini;
  if (!settingsPath(path, MAX_PATH * 2) || !loadIni(path, ini)) {
    settingsMissing(path);
    return false;
  }
  Ini after = ini;
  if (g_fpsFlag != FpsFlag::None) {
    applyFps(after, g_fpsFlag == FpsFlag::Default ? kDefaultFps : g_fpsValue);
  }
  if (g_introFlag >= 0) applyIntro(after, g_introFlag == 1);
  if (const DWORD e = saveIfChanged(path, ini, after)) return fileError(L"Could not write TdEngine.ini.", e);
  if (g_quiet) fwprintf(stderr, L"TdEngine.ini: %ls\n", describe(readSettings(after)).c_str());
  return true;
}

// After uninstalling: if the settings Shatter's setup changed differ from the backup, offer to put them back.
void offerSettingsRestore() {
  wchar_t path[MAX_PATH * 2], backup[MAX_PATH * 2];
  Ini now, before;
  if (!settingsPath(path, MAX_PATH * 2)) return;
  swprintf(backup, MAX_PATH * 2, L"%ls.shatter-backup", path);
  if (!loadIni(path, now) || !loadIni(backup, before)) return;
  const GameSettings a = readSettings(now), b = readSettings(before);
  if (a.smooth == b.smooth && fabs(a.maxFps - b.maxFps) < 1e-6 && fabs(a.minFps - b.minFps) < 1e-6 &&
      a.movies == b.movies)
    return;
  const std::wstring text = L"This setup also changed game settings.\n\nNow: " + describe(a) +
                            L".\nBefore Shatter: " + describe(b) + L".";
  const TASKDIALOG_BUTTON btn[] = {{1, L"Put them back"}, {2, L"Keep them"}};
  if (g_quiet || taskDialog(L"Put the game settings back?", text.c_str(), TD_INFORMATION_ICON, btn, 2, 0) != 1) return;
  Ini after = now;
  if (a.smooth != b.smooth) setKey(after, kGameEngine, L"bSmoothFrameRate", b.smooth ? L"TRUE" : L"FALSE");
  if (fabs(a.maxFps - b.maxFps) >= 1e-6) setKey(after, kGameEngine, L"MaxSmoothedFrameRate", formatFps(b.maxFps));
  if (fabs(a.minFps - b.minFps) >= 1e-6) setKey(after, kGameEngine, L"MinSmoothedFrameRate", formatFps(b.minFps));
  if (a.movies != b.movies) setKey(after, kMovies, L"StartupMovies", b.movies);
  if (const DWORD e = saveIni(path, after)) {
    fileError(L"Could not write TdEngine.ini.", e);
    return;
  }
  DeleteFileW(backup);  // the next change backs up the file as it is then
  info(L"Game settings put back", (L"Now: " + describe(readSettings(after)) + L".").c_str());
}

// The screen after installing or updating, with the Game settings button.
void installed(const wchar_t* dir, const wchar_t* text) {
  wchar_t main[128];
  swprintf(main, 128, L"Shatter %ls is installed", kVersion);
  if (g_quiet) {
    taskDialog(main, text, TD_INFORMATION_ICON, nullptr, 0, TDCBF_CLOSE_BUTTON);
    return;
  }
  const TASKDIALOG_BUTTON b[] = {{1003, L"Game settings..."}};
  if (taskDialog(main, text, TD_INFORMATION_ICON, b, 1, TDCBF_CLOSE_BUTTON) == 1003) gameSettings(dir);
}

// `added` (the marker path) when Binaries has no PhysXLoader.dll of its own (see the top of this file), else nullptr.
bool install(const wchar_t* dir, const wchar_t* dll, const wchar_t* backup, const wchar_t* added) {
  wchar_t text[1536];
  const bool haveBackup = GetFileAttributesW(backup) != INVALID_FILE_ATTRIBUTES;
  swprintf(text, 1536,
           L"Game folder:\n%ls\n\n%ls", dir,
           added        ? L"This copy of the game has no PhysXLoader.dll in this folder, only in PhysXLocal. Shatter's "
                          L"is added here and the PhysXLocal one is left as it is. Uninstalling removes it again."
           : haveBackup ? L"PhysXLoader.dll.original already exists there and is kept as it is."
                        : L"The original PhysXLoader.dll is kept as PhysXLoader.dll.original.");
  const TASKDIALOG_BUTTON b[] = {{IDOK, L"Install"}};
  wchar_t main[128];
  swprintf(main, 128, L"Install Shatter %ls?", kVersion);
  if (g_action != Action::Install && taskDialog(main, text, TD_INFORMATION_ICON, b, 1, TDCBF_CANCEL_BUTTON) != IDOK)
    return false;
  if (added) {
    const HANDLE f = CreateFileW(added, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return fileError(L"Could not write PhysXLoader.dll.shatter-added.", GetLastError());
    const char note[] = "Shatter added PhysXLoader.dll to this folder. Uninstalling it with ShatterSetup.exe deletes both.\r\n";
    DWORD written = 0;
    WriteFile(f, note, sizeof note - 1, &written, nullptr);
    CloseHandle(f);
  } else if (!haveBackup && !CopyFileW(dll, backup, TRUE)) {
    return fileError(L"Could not back up PhysXLoader.dll.", GetLastError());
  }
  if (const DWORD e = writeOurs(dll)) {
    if (added) DeleteFileW(added);
    return fileError(L"Could not write PhysXLoader.dll.", e);
  }
  applyFlags();
  swprintf(text, 1536,
           L"In:\n%ls\n\nStart the game as usual, with PhysX on in the graphics options.\n"
           L"To uninstall, run this setup again, or use Steam Verify / GOG Repair / EA app Repair.\n\n"
           L"Game settings: frame rate cap and intro movie (optional).",
           dir);
  installed(dir, text);
  return true;
}

bool update(const wchar_t* dir, const wchar_t* dll) {
  if (const DWORD e = writeOurs(dll)) return fileError(L"Could not write PhysXLoader.dll.", e);
  applyFlags();
  installed(dir, L"Updated.\n\nGame settings: frame rate cap and intro movie (optional).");
  return true;
}

bool uninstall(const wchar_t* dll, const wchar_t* backup, const wchar_t* added) {
  if (GetFileAttributesW(added) != INVALID_FILE_ATTRIBUTES) {  // installed into a Binaries without a PhysXLoader.dll
    if (!DeleteFileW(dll)) return fileError(L"Could not delete PhysXLoader.dll.", GetLastError());
    DeleteFileW(added);
    info(L"Shatter is uninstalled", L"Shatter's PhysXLoader.dll is removed; the game uses its PhysXLocal one again.");
    offerSettingsRestore();
    return true;
  }
  const LoaderInfo b = inspect(backup);
  if (b.kind != Kind::Original) {
    error(b.kind == Kind::Missing ? L"No backup of the original PhysXLoader.dll" : L"The backup is not the original",
          L"Restore the original file with your store instead:\n"
          L"- Steam: Properties > Installed Files > Verify integrity of game files\n"
          L"- GOG Galaxy: Manage installation > Verify / Repair\n"
          L"- EA app: Repair, in the game's menu in your Library");
    return false;
  }
  if (!MoveFileExW(backup, dll, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    return fileError(L"Could not restore PhysXLoader.dll.original.", GetLastError());
  info(L"Shatter is uninstalled", L"The original PhysXLoader.dll is back in place.");
  offerSettingsRestore();
  return true;
}

// The game's Binaries folder from a path to MirrorsEdge.exe or to the folder itself.
bool gameDir(const wchar_t* arg, wchar_t* dir, DWORD size) {
  wchar_t full[MAX_PATH * 2];
  if (!GetFullPathNameW(arg, MAX_PATH * 2, full, nullptr)) return false;
  const DWORD a = GetFileAttributesW(full);
  if (a == INVALID_FILE_ATTRIBUTES) return false;
  if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
    wchar_t* slash = wcsrchr(full, L'\\');
    if (!slash) return false;
    *slash = 0;
  }
  swprintf(dir, size, L"%ls", full);
  return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  wchar_t picked[MAX_PATH * 2] = L"";
  const wchar_t* start = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (!_wcsicmp(argv[i], L"/quiet")) g_quiet = true;
    else if (!_wcsicmp(argv[i], L"/install")) g_action = Action::Install;
    else if (!_wcsicmp(argv[i], L"/uninstall")) g_action = Action::Uninstall;
    else if (!_wcsicmp(argv[i], L"/skipintro")) g_introFlag = 0;
    else if (!_wcsicmp(argv[i], L"/intro")) g_introFlag = 1;
    else if (!_wcsnicmp(argv[i], L"/fps=", 5)) {
      const wchar_t* v = argv[i] + 5;
      g_fpsFlag = !_wcsicmp(v, L"default") ? FpsFlag::Default : FpsFlag::Value;
      g_fpsValue = _wtoi(v);
      if (g_fpsFlag == FpsFlag::Value && (g_fpsValue < 30 || g_fpsValue > 1000)) {
        fwprintf(stderr, L"/fps= takes 30 to 1000 or default\n");
        return 1;
      }
    }
    else start = argv[i];
  }
  if (g_quiet && !start) return 1;

  bool welcomed = false;
  for (;;) {
    if (!start) {
      if (!welcomed) {  // first screen without a path: say what to pick before the file dialog opens
        welcomed = true;
        const TASKDIALOG_BUTTON b[] = {{IDOK, L"Locate MirrorsEdge.exe\nIn the game's Binaries folder"}};
        wchar_t main[128];
        swprintf(main, 128, L"Install Shatter %ls for Mirror's Edge", kVersion);
        if (taskDialog(main,
                       L"Shatter replaces the game's PhysXLoader.dll. The original is kept as "
                       L"PhysXLoader.dll.original.",
                       TD_INFORMATION_ICON, b, 1, TDCBF_CANCEL_BUTTON, true) != IDOK)
          return 0;
      }
      if (!pickExe(picked, MAX_PATH * 2)) return 0;
      start = picked;
    }
    wchar_t dir[MAX_PATH * 2], exe[MAX_PATH * 2], dll[MAX_PATH * 2], backup[MAX_PATH * 2], added[MAX_PATH * 2],
        local[MAX_PATH * 2];
    const bool okDir = gameDir(start, dir, MAX_PATH * 2);
    start = nullptr;
    swprintf(exe, MAX_PATH * 2, L"%ls\\MirrorsEdge.exe", dir);
    swprintf(dll, MAX_PATH * 2, L"%ls\\PhysXLoader.dll", dir);
    swprintf(backup, MAX_PATH * 2, L"%ls\\PhysXLoader.dll.original", dir);
    swprintf(added, MAX_PATH * 2, L"%ls\\PhysXLoader.dll.shatter-added", dir);
    swprintf(local, MAX_PATH * 2, L"%ls\\PhysXLocal\\PhysXLoader.dll", dir);
    LoaderInfo li = inspect(dll);
    const bool addHere = li.kind == Kind::Missing && inspect(local).kind == Kind::Original;  // EA app layout
    if (addHere) li.kind = Kind::Original;
    if (!okDir || GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES || li.kind == Kind::Missing) {
      wchar_t text[1024];
      swprintf(text, 1024, L"%ls\n\nPick MirrorsEdge.exe in the game's Binaries folder (PhysXLoader.dll is next to it).",
               okDir ? dir : L"(path not found)");
      const TASKDIALOG_BUTTON b[] = {{IDRETRY, L"Choose again"}};
      if (g_quiet) fwprintf(stderr, L"MirrorsEdge.exe or PhysXLoader.dll not found: %ls\n", text);
      else if (taskDialog(okDir && GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES
                         ? L"No PhysXLoader.dll next to MirrorsEdge.exe"
                         : L"MirrorsEdge.exe not found",
                     text, TD_WARNING_ICON, b, 1, TDCBF_CLOSE_BUTTON) == IDRETRY)
        continue;
      return 1;
    }
    switch (li.kind) {
      case Kind::Original:
        if (g_action == Action::Uninstall) {
          error(L"Shatter is not installed", dir);
          return 1;
        }
        return install(dir, dll, backup, addHere ? added : nullptr) ? 0 : 1;
      case Kind::Shatter: {
        wchar_t main[128], text[1024];
        swprintf(main, 128, L"Shatter %ls is installed", li.version);
        swprintf(text, 1024, L"In:\n%ls", dir);
        wchar_t upd[192];  // command links get a second line each (the space is reserved for it either way)
        swprintf(upd, 192, L"%ls %ls\n%ls", wcscmp(li.version, kVersion) ? L"Update to" : L"Reinstall",
                 kVersion, wcscmp(li.version, kVersion) ? L"Replaces the installed version" : L"Repairs the installation");
        const TASKDIALOG_BUTTON b[] = {{1001, upd},
                                       {1002, L"Uninstall\nRestores the original PhysXLoader.dll"},
                                       {1003, L"Game settings\nFrame rate cap and intro movie"}};
        for (;;) {  // back to this screen after Game settings
          const int r = g_action == Action::Install     ? 1001
                        : g_action == Action::Uninstall ? 1002
                                                        : taskDialog(main, text, TD_INFORMATION_ICON, b, 3, TDCBF_CLOSE_BUTTON, true);
          if (r == 1001) return update(dir, dll) ? 0 : 1;
          if (r == 1002) return uninstall(dll, backup, added) ? 0 : 1;
          if (r != 1003) return 1;
          gameSettings(dir);
        }
      }
      default: {
        wchar_t text[1024];
        swprintf(text, 1024,
                 L"%ls\\PhysXLoader.dll is version %ls; Shatter expects the game's original 2.8.0.7.\n"
                 L"Nothing was changed.",
                 dir, li.version[0] ? li.version : L"(none)");
        error(L"Unknown PhysXLoader.dll", text);
        return 1;
      }
    }
    return 0;
  }
}
