# Changelog

## Unreleased

- **Login fix on Sunrise** (`OnlineFix=1`, default): the game's master names no longer lead to the server (game.trackmaniasunrise.com does not resolve, www.trackmaniasunrise.com points to a parking server: error 12031). Inside the game process only, a connection to one of them goes to Nadeo's old master server 91.121.54.245 (it still serves Sunrise) and each request keeps the name in its Host header; TLS 1.0 and no revocation check as on ESWC (error 12157). A hosts-file entry keeps working. The Horizon Client's login fix, the same code (also its new `onlinefix_names.h`). Not tested in the game yet.

## 1.3.1

- No Sounds slider: the games' own options have Volume and Music only, and the slider was not heard on ESWC. A `Sounds=` value in the ini is no longer put into the game. The Horizon Client's Game settings 1.1.1, the same code.

## 1.3.0: Legacy Essentials

New name: **Legacy Essentials** (formerly eswc-autosave). File names stay (`dinput8.dll`, `eswc_autosave.dll`, `eswc_autosave.ini`), your ini keeps working.

- **Controller fix** (ESWC, Sunrise, Original): a controller plugged in or switched on while the game runs is added at once, with the game's own bindings for it. The Horizon Client's controller fix 1.2.1, the same code. `ControllerFix=1` in the new `[controller]` section.
- **Menu**: `Ctrl+F2` switches Autosave, Best times only, Controller fix and Login fix, opens the replay folder and shows the version. Changes are written to the ini at once. `MenuKey` in the new `[menu]` section.
- **Volume, Music, Sounds, Window mode** (ESWC, Sunrise, Original) in the menu. Music and Sounds are the game's own volumes and change at once; Volume is the game's volume in Windows; Window mode (Windowed/Fullscreen) switches at once with the game's own switch. Your choice is kept in the new `[sound]` and `[display]` sections and put into the game's settings at every start. The Horizon Client's Game settings module, the same code.
- A short note in the top right corner when a replay is saved or a controller is added.
- **Login fix**: now the Horizon Client's file itself (it was a copy of this mod's code before), so a fix lands in both. Behaviour unchanged.
- **With the Horizon Client**: Legacy Essentials steps aside for the client's login fix, controller fix, game settings and menu. Autosave stays with Legacy Essentials (the client's Autosave module pauses itself for it, as before).
- The loader passes DirectInput on to a `dinput8.chain.dll` next to it (another mod's renamed `dinput8.dll`), and starts the mod after the game's own DirectInput call.
- Autosave switched off at start (`Enabled=0`) installs no hooks until it is switched on in the menu.

## 1.2.0

- TrackMania Sunrise (Win10 repack) and TrackMania Original (Online Patch v2 exe): solo autosave; online fix also for Original. The exe is now also checked by its file size.

## 1.1.0

- TMX copy for LAN/online replays.

## 1.0.1

- Split into a small loader (`dinput8.dll`) and the mod (`eswc_autosave.dll`). Behaviour unchanged.

## 1.0.0

- First release.
