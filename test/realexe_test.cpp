// Checks the vendored Horizon code against the real game exes on disk (read only, nothing is run):
//   - the controller fix's game table: PE timestamp, every code signature and table entry (GameBackend::verify) and
//     the late-pad bindings' signatures (Binder::verify)
//   - the login fix's import slots: ESWC 0x00aca340, Original 0x00a162d4 and Sunrise 0x00a1a2dc are the exe's
//     HttpOpenRequestA slot; Sunrise 0x00a1a2d8 InternetConnectA and 0x00a1a2d4 InternetCloseHandle
//   - the menu's way in: the exe imports Direct3DCreate9 or GetProcAddress
//   - keep bindings: the bytes at the profile load's reset call (Sunrise, Original)
//   - the game settings' table: PE timestamp, the code, pointer and reflection-member signatures of the volumes and
//     of the window mode and of the game's own fullscreen switch (gs::Game::audioOk / displayOk / toggleOk)
// The exe image is laid out at its link address 0x400000 in this process (the exes have no relocations).
//
//   realexe_test.exe <exe> eswc|sunrise|original [...]
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "../vendor/horizon/controller-hotplug/hp_game.h"
#include "../vendor/horizon/game-settings/gs_game.h"
#include "../vendor/horizon/host/keepbindings_sites.h"
#include "../src/games.h"
#include "../src/log.h"
#include "../src/shim/host/host.h"

#include <set>

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const char* what) {
    if (ok) g_pass++;
    else g_fail++;
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
}

static std::string g_lastLog;
static void logCb(void*, int level, const char* text) {
    g_lastLog = text;
    printf("      [%s] %s\n", level == 2 ? "error" : level == 1 ? "warn" : "info", text);
}

// RVA of the IAT slot of dll!fn, 0 if not imported
static DWORD iatRva(const BYTE* img, const char* dll, const char* fn) {
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(img + reinterpret_cast<const IMAGE_DOS_HEADER*>(img)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(img + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp(reinterpret_cast<const char*>(img + d->Name), dll) != 0) continue;
        const DWORD names = d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk; // on disk FirstThunk holds the names too
        auto* n = reinterpret_cast<const IMAGE_THUNK_DATA32*>(img + names);
        for (DWORD i = 0; n[i].u1.AddressOfData; i++) {
            if (IMAGE_SNAP_BY_ORDINAL32(n[i].u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(img + n[i].u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(byName->Name), fn) == 0) return d->FirstThunk + i * 4;
        }
    }
    return 0;
}

static bool runOne(const wchar_t* path, const char* game) {
    printf("===== %ls (%s)\n", path, game);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        check(false, "exe readable");
        return false;
    }
    std::vector<BYTE> file(GetFileSize(f, nullptr));
    DWORD got = 0;
    ReadFile(f, file.data(), static_cast<DWORD>(file.size()), &got, nullptr);
    CloseHandle(f);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(file.data() + reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data())->e_lfanew);
    const uintptr_t base = nt->OptionalHeader.ImageBase;
    check(base == 0x400000, "link address 0x400000");
    // at the link address if that range is free here, else anywhere (then the few pointers the table checks are
    // moved by the same distance, as a loader would relocate them; the comparison stays "dword == link value")
    BYTE* img = static_cast<BYTE*>(VirtualAlloc(reinterpret_cast<void*>(base), nt->OptionalHeader.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!img) img = static_cast<BYTE*>(VirtualAlloc(nullptr, nt->OptionalHeader.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!img) {
        check(false, "memory for the image");
        return false;
    }
    memcpy(img, file.data(), nt->OptionalHeader.SizeOfHeaders);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (sec[i].SizeOfRawData) memcpy(img + sec[i].VirtualAddress, file.data() + sec[i].PointerToRawData, sec[i].SizeOfRawData);
    const uintptr_t at = reinterpret_cast<uintptr_t>(img);
    printf("      image at %08X\n", static_cast<unsigned>(at));

    const hp::GameTable* t = hp::tableForTimestamp(nt->FileHeader.TimeDateStamp);
    check(t != nullptr, "controller fix: table for this PE timestamp");
    if (t) {
        if (at != base)
            for (int i = 0; i < t->ptrCount; i++) *reinterpret_cast<uint32_t*>(img + (t->ptrs[i].addr - base)) += static_cast<uint32_t>(at - base);
        hp::GameBackend backend(t, at, &logCb, nullptr);
        const bool v = backend.verify();
        char what[160];
        _snprintf_s(what, _TRUNCATE, "controller fix: %d code signatures + %d table entries match (%s)", t->codeCount, t->ptrCount, t->name);
        check(v, what);
        const hp::Binder* b = backend.binder();
        check(b && g_lastLog.find("bindings code checked") == 0, "controller fix: late-pad bindings code signatures match");
    }
    const gs::GameTable* g = gs::tableForTimestamp(nt->FileHeader.TimeDateStamp);
    check(g != nullptr, "game settings: table for this PE timestamp");
    if (g) {
        if (at != base) {
            std::set<uintptr_t> moved; // the two parts share some pointers: move each dword once
            for (const gs::Part* part : {&g->audio, &g->display, &g->toggle})
                for (int i = 0; i < part->ptrCount; i++)
                    if (moved.insert(part->ptrs[i].addr).second)
                        *reinterpret_cast<uint32_t*>(img + (part->ptrs[i].addr - base)) += static_cast<uint32_t>(at - base);
        }
        gs::Game settings(g, at, &logCb, nullptr);
        char what[160];
        _snprintf_s(what, _TRUNCATE, "game settings: volume signatures match (%d code, %d pointer, %d member; %s)", g->audio.codeCount,
                    g->audio.ptrCount, g->audio.paramCount, g->name);
        check(settings.audioOk(), what);
        _snprintf_s(what, _TRUNCATE, "game settings: window-mode signatures match (%d code, %d pointer, %d member)", g->display.codeCount,
                    g->display.ptrCount, g->display.paramCount);
        check(settings.displayOk(), what);
        _snprintf_s(what, _TRUNCATE, "game settings: live window-mode switch signatures match (%d code, %d vtable slots)", g->toggle.codeCount,
                    g->toggle.ptrCount);
        check(settings.toggleOk(), what);
    }
    {
        const DWORD want = strcmp(game, "eswc") == 0       ? 0x00aca340 - 0x400000
                           : strcmp(game, "original") == 0 ? 0x00a162d4 - 0x400000
                                                           : 0x00a1a2dc - 0x400000;
        check(iatRva(img, "WININET.dll", "HttpOpenRequestA") == want, "login fix: HttpOpenRequestA import slot as in essentials.cpp");
    }
    if (strcmp(game, "sunrise") == 0) {
        check(iatRva(img, "WININET.dll", "InternetConnectA") == 0x00a1a2d8 - 0x400000,
              "login fix: InternetConnectA import slot as in essentials.cpp (Sunrise master names)");
        check(iatRva(img, "WININET.dll", "InternetCloseHandle") == 0x00a1a2d4 - 0x400000,
              "login fix: InternetCloseHandle import slot as in essentials.cpp");
    }
    if (strcmp(game, "sunrise") == 0 || strcmp(game, "original") == 0) {
        const horizon::keepbind::Site& s = strcmp(game, "original") == 0 ? horizon::keepbind::original() : horizon::keepbind::sunrise();
        check(horizon::keepbind::check(s, img + (s.call - 10 - 0x400000)) == horizon::keepbind::State::Original,
              "keep bindings: the profile load's reset call is the one the fix redirects");
        // the real install on this copy of the image (no app yet: its global is 0 in the file)
        const uintptr_t imgBase = reinterpret_cast<uintptr_t>(img);
        const bool orig = strcmp(game, "original") == 0;
        check(horizon::keepBindingsInstall(imgBase, orig) &&
                  horizon::keepbind::check(s, img + (s.call - 10 - 0x400000)) == horizon::keepbind::State::Kept,
              "keep bindings: installed on the image, the call now goes to the defaults pass");
        check(horizon::keepBindingsInstall(imgBase, orig), "keep bindings: a second install sees it done and changes nothing");
        static uint8_t fakeApp[0x200]; // a fake app with 1 loaded profile (outside the image: nothing else is touched)
        const uint32_t app = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fakeApp));
        memcpy(img + (s.appGlobal - 0x400000), &app, 4);
        const uint32_t one = 1;
        memcpy(fakeApp + s.appProfiles, &one, 4);
        uint8_t back[5];
        horizon::keepbind::callBytes(s.call, s.resetFn, back);
        memcpy(img + (s.call - 0x400000), back, 5);
        check(!horizon::keepBindingsInstall(imgBase, orig) &&
                  horizon::keepbind::check(s, img + (s.call - 10 - 0x400000)) == horizon::keepbind::State::Original,
              "keep bindings: profiles already loaded -> too late, nothing changed");
    }
    {
        // Crazy mode autosave (games.h CrazySite): the two race classes' vtables, the race end slot, the shared
        // finish slot before it, both prologues and the new-ghost counter the Crazy race end raises.
        const autosave::Game gm = strcmp(game, "eswc") == 0       ? autosave::Game::Eswc
                                  : strcmp(game, "original") == 0 ? autosave::Game::Original
                                                                  : autosave::Game::Sunrise;
        const autosave::CrazySite* c = autosave::crazySite(gm);
        auto u32 = [&](uintptr_t link) { return *reinterpret_cast<const uint32_t*>(img + (link - 0x400000)); };
        auto bytes = [&](uintptr_t link) { return img + (link - 0x400000); };
        check(u32(c->vtSolo + 4 * c->endSlot) == c->soloEnd, "crazy: CTrackManiaRace1P vtable race-end slot = the solo race end");
        check(u32(c->vtCrazy + 4 * c->endSlot) == c->crazyEnd, "crazy: CTrackManiaRace1PCrazy overrides that slot with its own race end");
        check(u32(c->vtSolo + 4 * (c->endSlot - 1)) == c->finishCaller && u32(c->vtCrazy + 4 * (c->endSlot - 1)) == c->finishCaller,
              "crazy: the slot before it is the same function in both classes");
        const uint8_t* fc = bytes(c->finishCaller + 9);
        check(fc[0] == 0xE8 && c->finishCaller + 9 + 5 + *reinterpret_cast<const int32_t*>(fc + 1) == c->onFinishLine,
              "crazy: that function calls OnFinishLine (our finish hook sees Crazy finishes)");
        check(memcmp(bytes(c->crazyEnd), autosave::kCrazyEndPrologue, sizeof(autosave::kCrazyEndPrologue)) == 0 &&
                  memcmp(bytes(c->soloEnd), autosave::kCrazyEndPrologue, sizeof(autosave::kCrazyEndPrologue)) == 0,
              "crazy: Crazy race end prologue 83 ec 08 53 55 56 57 (as the solo race end)");
        // mov edi, [esi+newGhostCount] right after the call that adds the new ghost
        bool counter = false;
        const uint8_t* ce = bytes(c->crazyEnd);
        for (int i = 0; i < 0x160 && !counter; i++)
            counter = ce[i] == 0x8B && ce[i + 1] == 0xBE && *reinterpret_cast<const uint32_t*>(ce + i + 2) == c->newGhostCount &&
                      ce[i - 5] == 0xE8;
        check(counter, "crazy: the race end reads the new-ghost counter after adding the ghost");
        // the base the Crazy race end reads the replay from: app+0x130 (Sunrise/Original), app+0x144 (ESWC)
        const uint32_t appReplay = gm == autosave::Game::Eswc ? 0x144 : static_cast<uint32_t>(autosave::legacy_off::kAppReplay);
        bool replay = false;
        for (int i = 0; i < 0x160 && !replay; i++) replay = ce[i] == 0x8B && ce[i + 1] == 0xBA && *reinterpret_cast<const uint32_t*>(ce + i + 2) == appReplay;
        check(replay, "crazy: the race end takes the run's replay from the app member the autosave saves");
        if (const autosave::LegacyGame* lg = autosave::legacyGame(gm))
            check(lg->soloRaceEnd == c->soloEnd && lg->onFinishLine == c->onFinishLine, "crazy: same solo race end and OnFinishLine as the legacy table");
    }
    const bool d3d = iatRva(img, "d3d9.dll", "Direct3DCreate9") != 0;
    const bool gpa = iatRva(img, "KERNEL32.dll", "GetProcAddress") != 0;
    printf("      imports: Direct3DCreate9 %s, GetProcAddress %s\n", d3d ? "yes" : "no", gpa ? "yes" : "no");
    check(d3d || gpa, "menu: the exe reaches Direct3D through an import the overlay patches");
    VirtualFree(img, 0, MEM_RELEASE);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    autosave::logOpen(); // the keep-bindings lines: eswc_autosave.log next to this exe
    if (argc < 3) {
        printf("realexe_test.exe <exe> eswc|sunrise|original [...]\n");
        return 2;
    }
    for (int i = 1; i + 1 < argc; i += 2) {
        char game[16];
        WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, game, sizeof(game), nullptr, nullptr);
        runOne(argv[i], game);
    }
    printf("realexe_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
