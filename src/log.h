#pragma once

namespace autosave {

// Opens eswc_autosave.log next to the exe (appends). Safe to call more than once.
void logOpen();
void logLine(const char* fmt, ...);
// At process exit (DllMain): a thread that died holding the log's lock must not stop the line.
void logLineAtExit(const char* fmt, ...);

} // namespace autosave

#define AS_LOG(...) ::autosave::logLine(__VA_ARGS__)
