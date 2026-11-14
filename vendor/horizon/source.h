// Written by tools/sync-horizon.ps1. The files in vendor\horizon\ are unchanged copies from these commits of
// horizon-host (cheatoskar/horizon-client); do not edit them here, fix them there and sync again.
#pragma once
// login fix, controller fix, Dear ImGui
#define HORIZON_SOURCE_COMMIT "4bdff8ab2d4d5fe6d4222606ce54199c0cafcf5b"
#define HORIZON_SOURCE_SHORT "4bdff8a"
#define HORIZON_SOURCE_DATE "2026-11-14"
// branches holding it when synced: pad-bindings-diag
// subject: Controller fix: log the pads' bindings at start (diagnostics only)
// game settings (volumes, window mode)
#define HORIZON_SETTINGS_COMMIT "fc5c4358aeb13a095f57cba2330173f49aa44428"
#define HORIZON_SETTINGS_SHORT "fc5c435"
#define HORIZON_SETTINGS_DATE "2026-11-13"
// branches holding it when synced: fix-0.3.1-game, pad-bindings-diag, readme-voice, sunrise-login
// subject: Game settings 1.1.1: no Sounds slider
