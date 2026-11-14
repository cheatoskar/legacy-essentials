#pragma once
// Pure helpers of the profile diagnostics (profilediag.cpp), header only so test\essentials_test.cpp tests them.
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include <string>
#include <vector>

#include "lzo.h"

namespace autosave {
namespace profilepath {

// What UAC file virtualization uses for a path: %LOCALAPPDATA%\VirtualStore\<path without the drive>.
// "C:\Program Files (x86)\TM\GameData\Profiles" -> "<localAppData>\VirtualStore\Program Files (x86)\TM\GameData\Profiles".
// Empty for a path without a drive letter (UNC) or an empty localAppData.
inline std::wstring virtualStore(const std::wstring& path, const std::wstring& localAppData) {
    if (localAppData.empty() || path.size() < 3 || path[1] != L':' || path[2] != L'\\') return std::wstring();
    std::wstring r = localAppData;
    if (r.back() != L'\\') r += L'\\';
    return r + L"VirtualStore\\" + path.substr(3);
}

// path is folder or inside it (case-insensitive, whole folder names: "C:\Program Files (x86)" is not inside
// "C:\Program Files"). An empty folder matches nothing.
inline bool under(const std::wstring& path, std::wstring folder) {
    while (!folder.empty() && folder.back() == L'\\') folder.pop_back();
    if (folder.empty() || path.size() < folder.size()) return false;
    if (_wcsnicmp(path.c_str(), folder.c_str(), folder.size()) != 0) return false;
    return path.size() == folder.size() || path[folder.size()] == L'\\';
}

// "Name.Profile.Gbx" (any case)
inline bool isProfileFile(const wchar_t* name) {
    static const wchar_t kExt[] = L".Profile.Gbx";
    const size_t n = name ? wcslen(name) : 0, e = sizeof(kExt) / sizeof(kExt[0]) - 1;
    return n > e && _wcsicmp(name + n - e, kExt) == 0;
}

// GBX body compression from the file's first bytes: "GBX", version (uint16), then for version >= 3 the format
// 'B', refs 'U'/'C', body 'U'/'C'. 1 = compressed, 0 = not, -1 = not a GBX header this knows.
inline int gbxBodyCompressed(const uint8_t* d, size_t n) {
    if (n < 8 || d[0] != 'G' || d[1] != 'B' || d[2] != 'X') return -1;
    const unsigned version = d[3] | (d[4] << 8);
    if (version < 3 || d[5] != 'B') return -1;
    return d[7] == 'C' ? 1 : d[7] == 'U' ? 0 : -1;
}

constexpr uint32_t kMaxBody = 4u << 20; // a profile body is a few hundred KB at most

// The decompressed body of a GBX v6 file with a compressed body ("GBX" u16 6 "BUCR", then u32 class, u32 userDataSize
// + user data, u32 numNodes, u32 numExternal (0), u32 bodySize, u32 compressedSize, LZO1X data). 1: out holds the
// body; 0: the body is not compressed (search the file itself); -1: not readable, *why says why.
inline int gbxBody(const uint8_t* d, size_t n, std::vector<uint8_t>& out, const char** why) {
    out.clear();
    const int c = gbxBodyCompressed(d, n);
    if (c == 0) return 0;
    *why = "not a GBX header this knows";
    if (c < 0) return -1;
    const unsigned version = d[3] | (d[4] << 8);
    if (version != 6) {
        *why = "GBX version is not 6";
        return -1;
    }
    size_t p = 9;
    auto u32 = [&](uint32_t* v) {
        if (n < 4 || p > n - 4) return false;
        *v = static_cast<uint32_t>(d[p]) | (static_cast<uint32_t>(d[p + 1]) << 8) | (static_cast<uint32_t>(d[p + 2]) << 16) |
             (static_cast<uint32_t>(d[p + 3]) << 24);
        p += 4;
        return true;
    };
    uint32_t cls = 0, user = 0, nodes = 0, ext = 0, size = 0, comp = 0;
    *why = "truncated header";
    if (!u32(&cls) || !u32(&user) || user > n - p) return -1;
    p += user;
    if (!u32(&nodes) || !u32(&ext)) return -1;
    if (ext) {
        *why = "external references, body not read";
        return -1;
    }
    if (!u32(&size) || !u32(&comp)) return -1;
    if (size == 0 || size > kMaxBody || comp > n - p) {
        *why = "body size out of range";
        return -1;
    }
    out.resize(size);
    if (!lzo::decompress(d + p, comp, out.data(), size, why)) {
        out.clear();
        return -1;
    }
    return 1;
}

// the GUID text ("fe7b39e0-cfa8-11f1-8001-444553540000") appears in the bytes, any case
inline bool containsText(const uint8_t* d, size_t n, const std::string& text) {
    const size_t m = text.size();
    if (!m || n < m) return false;
    auto low = [](uint8_t c) { return static_cast<uint8_t>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); };
    for (size_t i = 0; i + m <= n; i++) {
        size_t k = 0;
        while (k < m && low(d[i + k]) == low(static_cast<uint8_t>(text[k]))) k++;
        if (k == m) return true;
    }
    return false;
}

} // namespace profilepath
} // namespace autosave
