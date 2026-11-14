// The profile diagnostics (src/profilediag.cpp) on a profile folder next to this exe (build\test\GameData\Profiles,
// made and removed here): the start lines, the pad check, a save seen by the watcher, the exit line. About 17 s
// (the watcher looks every 5 s). Log: eswc_autosave.log next to this exe.
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <string>

#include "../src/log.h"
#include "../src/profilediag.h"

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? g_pass : g_fail)++;
}

static std::wstring g_logPath;
static long long g_logStart = 0;
static std::string logSince() {
    std::string r;
    HANDLE h = CreateFileW(g_logPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return r;
    LARGE_INTEGER sz = {}, at = {};
    GetFileSizeEx(h, &sz);
    at.QuadPart = g_logStart;
    if (sz.QuadPart > g_logStart && SetFilePointerEx(h, at, nullptr, FILE_BEGIN)) {
        r.resize(static_cast<size_t>(sz.QuadPart - g_logStart));
        DWORD got = 0;
        ReadFile(h, &r[0], static_cast<DWORD>(r.size()), &got, nullptr);
        r.resize(got);
    }
    CloseHandle(h);
    return r;
}
static bool logHas(const char* part) { return logSince().find(part) != std::string::npos; }

static void writeProfile(const std::wstring& path, const char* body) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const char head[] = {'G', 'B', 'X', 6, 0, 'B', 'U', 'U', 'R'};
    DWORD w = 0;
    WriteFile(h, head, sizeof(head), &w, nullptr);
    WriteFile(h, body, static_cast<DWORD>(strlen(body)), &w, nullptr);
    CloseHandle(h);
}

int main() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    *wcsrchr(exe, L'\\') = 0;
    const std::wstring dir = std::wstring(exe) + L"\\GameData\\Profiles", file = dir + L"\\Tester.Profile.Gbx";
    CreateDirectoryW((std::wstring(exe) + L"\\GameData").c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    writeProfile(file, "..pads: fe7b39e0-cfa8-11f1-8001-444553540000 ..");
    Sleep(2100); // older than the settle time

    g_logPath = std::wstring(exe) + L"\\eswc_autosave.log";
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &a))
        g_logStart = (static_cast<long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    autosave::logOpen();

    autosave::profilediag::start();
    check(logHas("\\GameData\\Profiles (exists, under Program Files: no, this process may add files there: yes)"),
          "start: the folder, exists, writable");
    check(logHas("profiles: UAC file virtualization for this process: "), "start: virtualization");
    check(logHas("profiles: Tester.Profile.Gbx: 56 bytes, written ") && logHas("this process may write it: yes"), "start: the profile");

    autosave::profilediag::setPads({"fe7b39e0-cfa8-11f1-8001-444553540000", "00000000-0000-0000-0000-000000000001"});
    Sleep(6000);
    check(logHas("profiles: Tester.Profile.Gbx: pad fe7b39e0 named, pad 00000000 NOT named"), "pads: checked in every profile once");

    writeProfile(file, "..no pad here, a little longer..");
    Sleep(11000);
    check(logHas("profiles: profile saved by the game: Tester.Profile.Gbx "), "watcher: the save is logged");
    check(logHas(", 41 bytes") && logHas("after its save at ") && logHas(": pad fe7b39e0 NOT named, pad 00000000 NOT named"),
          "watcher: size, and the pad check once the file settled");
    check(logSince().find("56 bytes, written") == logSince().rfind("56 bytes, written"), "no other profile lines (no contents)");

    writeProfile(file, "..saved at exit..");
    autosave::profilediag::quitSeen();
    autosave::profilediag::processExit();
    check(logHas("game: WM_QUIT on the game thread"), "WM_QUIT line");
    check(logHas("profiles: profile saved since the last check: Tester.Profile.Gbx "), "exit: the last save is logged");
    check(logHas("game closed normally (process exit after 0 min ") && logHas("WM_QUIT seen: yes; profile saves seen: 2)"),
          "exit: the closing line");

    DeleteFileW(file.c_str());
    RemoveDirectoryW(dir.c_str());
    RemoveDirectoryW((std::wstring(exe) + L"\\GameData").c_str());
    printf("profilediag_test: %d passed, %d failed\n", g_pass, g_fail);
    fflush(stdout);
    ExitProcess(g_fail ? 1 : 0); // the watcher thread never ends by itself
}
