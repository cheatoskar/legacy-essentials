// Profile diagnostics, log only (profilediag.h). Nothing here writes to the game or to a file: folders are listed,
// write access is asked of the security descriptor (AccessCheck, never by opening for write: in a virtualized
// process that alone would make a VirtualStore copy), and a profile is only opened for read, shared, for the pad
// GUID check after the game finished writing it. No profile contents are logged (they hold the login).
#include "profilediag.h"

#include <windows.h>

#include <stdio.h>

#include "log.h"
#include "profilepath.h"

namespace autosave {
namespace profilediag {
namespace {

constexpr DWORD kPollMs = 5000;
constexpr ULONGLONG kSettleMs = 2000;  // a profile is read for the GUID check once it was not written for this long
constexpr DWORD kMaxRead = 8u << 20;   // profiles are a few KB to a few hundred KB (body capped in profilepath.h)

struct FileState {
    bool store; // in the VirtualStore copy of the folder
    std::wstring name;
    FILETIME written;
    uint64_t size;
    DWORD attrs;
};

CRITICAL_SECTION g_cs;
bool g_init = false;
std::wstring g_dir, g_store;
std::vector<FileState> g_files;         // watcher thread (processExit only with the lock)
std::vector<std::string> g_guids;       // lock
bool g_guidsNew = false;                // lock: check every profile once
std::vector<FileState> g_pending;       // watcher thread: saved, GUID check when settled
volatile LONG g_saves = 0;
volatile LONG g_quit = 0;
ULONGLONG g_startTick = 0;

std::string utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::string timeText(const FILETIME& ft) {
    FILETIME local;
    SYSTEMTIME t;
    if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &t)) return "?";
    char b[32];
    sprintf_s(b, "%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

ULONGLONG ageMs(const FILETIME& ft) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const ULONGLONG a = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    const ULONGLONG b = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return a > b ? (a - b) / 10000 : 0;
}

std::wstring envVar(const wchar_t* name) {
    wchar_t b[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(name, b, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(b) : std::wstring();
}

const std::wstring& dirOf(const FileState& f) { return f.store ? g_store : g_dir; }

void listInto(const std::wstring& dir, bool store, std::vector<FileState>& out) {
    if (dir.empty()) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*.Gbx").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !profilepath::isProfileFile(fd.cFileName)) continue;
        out.push_back(FileState{store, fd.cFileName, fd.ftLastWriteTime,
                                (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow, fd.dwFileAttributes});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::vector<FileState> listAll() {
    std::vector<FileState> v;
    listInto(g_dir, false, v);
    listInto(g_store, true, v);
    return v;
}

// May this process write `path` (file) or add files to it (folder)? From its security descriptor and the process
// token; nothing is opened for write. "yes", "no" or "unknown".
const char* mayWrite(const std::wstring& path, bool folder) {
    DWORD need = 0;
    const SECURITY_INFORMATION si = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    GetFileSecurityW(path.c_str(), si, nullptr, 0, &need);
    if (!need) return "unknown";
    std::vector<BYTE> sd(need);
    if (!GetFileSecurityW(path.c_str(), si, sd.data(), need, &need)) return "unknown";
    HANDLE tok = nullptr, imp = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &tok)) return "unknown";
    const BOOL dup = DuplicateToken(tok, SecurityImpersonation, &imp);
    CloseHandle(tok);
    if (!dup) return "unknown";
    GENERIC_MAPPING map = {FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
    DWORD want = folder ? FILE_ADD_FILE : FILE_WRITE_DATA;
    MapGenericMask(&want, &map);
    PRIVILEGE_SET ps;
    DWORD psLen = sizeof(ps), granted = 0;
    BOOL ok = FALSE;
    const BOOL checked = AccessCheck(sd.data(), imp, want, &map, &ps, &psLen, &granted, &ok);
    CloseHandle(imp);
    if (!checked) return "unknown";
    return ok ? "yes" : "no";
}

DWORD tokenDword(TOKEN_INFORMATION_CLASS what) {
    HANDLE tok = nullptr;
    DWORD v = 0xffffffffu, len = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (!GetTokenInformation(tok, what, &v, sizeof(v), &len)) v = 0xffffffffu;
        CloseHandle(tok);
    }
    return v;
}
const char* yesNo(DWORD v) { return v == 0xffffffffu ? "unknown" : v ? "yes" : "no"; }

std::string fileLabel(const FileState& f) { return utf8(f.name) + (f.store ? " (VirtualStore copy)" : ""); }

// Does the profile name each pad (its bindings are stored under the instance GUID)? Read only, shared.
std::string padCheck(const FileState& f, const std::vector<std::string>& guids) {
    HANDLE h = CreateFileW((dirOf(f) + L"\\" + f.name).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "not readable (error " + std::to_string(GetLastError()) + ")";
    LARGE_INTEGER sz = {};
    std::vector<uint8_t> d;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart <= kMaxRead) {
        d.resize(static_cast<size_t>(sz.QuadPart));
        DWORD got = 0;
        if (!ReadFile(h, d.data(), static_cast<DWORD>(d.size()), &got, nullptr)) got = 0;
        d.resize(got);
    }
    CloseHandle(h);
    if (d.empty()) return "empty or too large, not checked";
    // the GUIDs are in the body; Sunrise compresses it (an LZO back-reference can cut the GUID in the file itself)
    std::vector<uint8_t> body;
    const char* why = "";
    const int b = profilepath::gbxBody(d.data(), d.size(), body, &why);
    if (b < 0) return std::string("body not readable (") + why + "), pads not checked";
    const std::vector<uint8_t>& text = b == 1 ? body : d;
    std::string r;
    for (const auto& g : guids) {
        if (!r.empty()) r += ", ";
        r += "pad " + g.substr(0, 8) + (profilepath::containsText(text.data(), text.size(), g) ? " named" : " NOT named");
    }
    return r.empty() ? "no pads to check" : r;
}

DWORD WINAPI watch(void*) {
    for (;;) {
        Sleep(kPollMs);
        std::vector<FileState> now = listAll();
        std::vector<std::string> guids;
        bool checkAll = false;
        EnterCriticalSection(&g_cs);
        for (const auto& f : now) {
            const FileState* old = nullptr;
            for (const auto& o : g_files)
                if (o.store == f.store && _wcsicmp(o.name.c_str(), f.name.c_str()) == 0) old = &o;
            if (old && CompareFileTime(&old->written, &f.written) == 0 && old->size == f.size) continue;
            InterlockedIncrement(&g_saves);
            AS_LOG("profiles: profile %s by the game: %s %s, %llu bytes", old ? "saved" : "created", fileLabel(f).c_str(),
                   timeText(f.written).c_str(), static_cast<unsigned long long>(f.size));
            g_pending.push_back(f);
        }
        for (const auto& o : g_files) {
            bool there = false;
            for (const auto& f : now)
                if (o.store == f.store && _wcsicmp(o.name.c_str(), f.name.c_str()) == 0) there = true;
            if (!there) AS_LOG("profiles: profile removed: %s", fileLabel(o).c_str());
        }
        g_files = now;
        guids = g_guids;
        checkAll = g_guidsNew;
        g_guidsNew = false;
        LeaveCriticalSection(&g_cs);
        if (guids.empty()) continue; // the pads are not known yet: saved profiles wait in g_pending
        if (checkAll) {
            for (const auto& f : now) AS_LOG("profiles: %s: %s", fileLabel(f).c_str(), padCheck(f, guids).c_str());
            g_pending.clear();
            continue;
        }
        for (size_t i = 0; i < g_pending.size();) {
            const FileState& f = g_pending[i];
            if (ageMs(f.written) < kSettleMs) { // the game may still be writing it
                i++;
                continue;
            }
            AS_LOG("profiles: %s after its save at %s: %s", fileLabel(f).c_str(), timeText(f.written).c_str(), padCheck(f, guids).c_str());
            g_pending.erase(g_pending.begin() + static_cast<ptrdiff_t>(i));
        }
    }
}

} // namespace

void start() {
    if (g_init) return;
    InitializeCriticalSection(&g_cs);
    g_init = true;
    g_startTick = GetTickCount64();
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* slash = (n && n < MAX_PATH) ? wcsrchr(exe, L'\\') : nullptr;
    if (!slash) {
        AS_LOG("profiles: exe path unknown, no profile diagnostics");
        return;
    }
    *slash = 0;
    g_dir = std::wstring(exe) + L"\\GameData\\Profiles";
    const DWORD a = GetFileAttributesW(g_dir.c_str());
    const bool exists = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    bool pf = false;
    for (const wchar_t* v : {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432"})
        if (profilepath::under(g_dir, envVar(v))) pf = true;
    AS_LOG("profiles: folder %s (%s, under Program Files: %s, this process may add files there: %s)", utf8(g_dir).c_str(),
           exists ? "exists" : "MISSING", pf ? "yes" : "no", exists ? mayWrite(g_dir, true) : "-");
    AS_LOG("profiles: UAC file virtualization for this process: %s, elevated: %s", yesNo(tokenDword(TokenVirtualizationEnabled)),
           yesNo(tokenDword(TokenElevation)));
    const std::wstring store = profilepath::virtualStore(g_dir, envVar(L"LOCALAPPDATA"));
    const DWORD sa = store.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(store.c_str());
    if (sa != INVALID_FILE_ATTRIBUTES && (sa & FILE_ATTRIBUTE_DIRECTORY)) {
        g_store = store;
        AS_LOG("profiles: VirtualStore copy of the folder EXISTS: %s (the game reads and writes there when virtualized)",
               utf8(store).c_str());
    } else {
        AS_LOG("profiles: no VirtualStore copy of the folder (%s)", store.empty() ? "no drive path" : utf8(store).c_str());
    }
    g_files = listAll();
    if (g_files.empty()) AS_LOG("profiles: no *.Profile.Gbx there");
    for (const auto& f : g_files) {
        const char* w = (f.attrs & FILE_ATTRIBUTE_READONLY) ? "no (read-only attribute)" : mayWrite(dirOf(f) + L"\\" + f.name, false);
        AS_LOG("profiles: %s: %llu bytes, written %s, this process may write it: %s", fileLabel(f).c_str(),
               static_cast<unsigned long long>(f.size), timeText(f.written).c_str(), w);
    }
    HANDLE t = CreateThread(nullptr, 0, &watch, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else AS_LOG("profiles: watcher thread failed (%lu): saves are not logged", GetLastError());
}

void setPads(const std::vector<std::string>& guids) {
    if (!g_init || guids.empty()) return;
    EnterCriticalSection(&g_cs);
    g_guids = guids;
    g_guidsNew = true;
    LeaveCriticalSection(&g_cs);
}

void quitSeen() {
    if (InterlockedExchange(&g_quit, 1) == 0) AS_LOG("game: WM_QUIT on the game thread (the game is closing by itself)");
}

void processExit() {
    if (!g_init) return;
    // Other threads are gone; one may have died holding a lock, so only try (and log without the log's lock).
    if (TryEnterCriticalSection(&g_cs)) {
        for (const auto& f : listAll()) {
            const FileState* old = nullptr;
            for (const auto& o : g_files)
                if (o.store == f.store && _wcsicmp(o.name.c_str(), f.name.c_str()) == 0) old = &o;
            if (old && CompareFileTime(&old->written, &f.written) == 0 && old->size == f.size) continue;
            g_saves++;
            logLineAtExit("profiles: profile %s since the last check: %s %s, %llu bytes", old ? "saved" : "created",
                          fileLabel(f).c_str(), timeText(f.written).c_str(), static_cast<unsigned long long>(f.size));
        }
        LeaveCriticalSection(&g_cs);
    }
    const ULONGLONG s = (GetTickCount64() - g_startTick) / 1000;
    logLineAtExit("game closed normally (process exit after %llu min %llu s; WM_QUIT seen: %s; profile saves seen: %ld)", s / 60,
                  s % 60, g_quit ? "yes" : "no", g_saves);
}

} // namespace profilediag
} // namespace autosave
