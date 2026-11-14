#pragma once
// Profile diagnostics (log only, nothing in the game or on disk is changed): where the game's profiles are, whether
// this process may write there (UAC VirtualStore), when the game saves a profile and whether the saved file names
// the pads (their bindings are stored under the DirectInput instance GUID), and whether the game closed normally.
// Written for "my pad bindings are reset at every start": the next log answers it without questions.
#include <string>
#include <vector>

namespace autosave {
namespace profilediag {

// At start (any thread): logs the folder <exe folder>\GameData\Profiles, its VirtualStore copy and the profiles in
// both, then watches them on its own thread (a directory listing every 5 s; the files are never opened for write).
void start();
// Game thread, once the controller fix logged the bindings at start: the pads' instance GUIDs (lower case). Each
// profile is then checked once, and again after every save, for whether it names these pads.
void setPads(const std::vector<std::string>& guids);
// The game thread took WM_QUIT: the game is closing by itself.
void quitSeen();
// DllMain DLL_PROCESS_DETACH while the process exits (ExitProcess; a kill or crash never gets here).
void processExit();

} // namespace profilediag
} // namespace autosave
