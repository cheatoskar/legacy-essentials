// Plain LZO1X decompressor (same algorithm as strip_ghosts.py), every access bounds-checked. Moved here from
// tmxcopy.cpp unchanged; also used by the profile diagnostics (profilepath.h).
#include "lzo.h"

#include <string.h>

namespace autosave {
namespace lzo {

bool decompress(const uint8_t* src, size_t srcLen, uint8_t* out, size_t outLen, const char** err) {
    size_t ip = 0, op = 0;
    *err = "lzo: truncated stream";
    auto literals = [&](size_t k) {
        if (k > srcLen - ip || k > outLen - op) return false;
        memcpy(out + op, src + ip, k);
        ip += k;
        op += k;
        return true;
    };
    auto match = [&](size_t dist, size_t k) {
        if (dist == 0 || dist > op || k > outLen - op) return false;
        for (size_t i = 0; i < k; i++, op++) out[op] = out[op - dist];
        return true;
    };
    auto extLen = [&](size_t base, size_t* t) {
        size_t v = 0;
        while (ip < srcLen && src[ip] == 0) {
            v += 255;
            ip++;
            if (v > outLen + 255) return false;
        }
        if (ip >= srcLen) return false;
        v += base + src[ip++];
        *t = v;
        return true;
    };
    if (srcLen < 1) return false;
    size_t t = src[ip];
    size_t state = 0; // literals copied by the last instruction (0, 1-3, or 4 = 4+)
    if (t > 17) {
        ip++;
        t -= 17;
        if (!literals(t)) return false;
        state = t >= 4 ? 4 : t;
    }
    for (;;) {
        if (ip >= srcLen) return false;
        t = src[ip++];
        size_t s = 0;
        if (t < 16) {
            if (state == 0) {
                if (t == 0 && !extLen(15, &t)) return false;
                if (!literals(t + 3)) return false;
                state = 4;
                continue;
            }
            if (ip >= srcLen) return false;
            const size_t hi = static_cast<size_t>(src[ip++]) << 2;
            const bool ok = state == 4 ? match(1 + 0x0800 + (t >> 2) + hi, 3) : match(1 + (t >> 2) + hi, 2);
            if (!ok) { *err = "lzo: bad match"; return false; }
            s = t & 3;
        } else if (t >= 64) {
            if (ip >= srcLen) return false;
            const size_t dist = 1 + ((t >> 2) & 7) + (static_cast<size_t>(src[ip++]) << 3);
            if (!match(dist, (t >> 5) - 1 + 2)) { *err = "lzo: bad match"; return false; }
            s = t & 3;
        } else if (t >= 32) {
            size_t len = t & 31;
            if (len == 0 && !extLen(31, &len)) return false;
            if (srcLen - ip < 2) return false;
            const size_t d = src[ip] | (static_cast<size_t>(src[ip + 1]) << 8);
            ip += 2;
            if (!match(1 + (d >> 2), len + 2)) { *err = "lzo: bad match"; return false; }
            s = d & 3;
        } else {
            size_t len = t & 7;
            if (len == 0 && !extLen(7, &len)) return false;
            if (srcLen - ip < 2) return false;
            const size_t d = src[ip] | (static_cast<size_t>(src[ip + 1]) << 8);
            ip += 2;
            const size_t dist = ((t & 8) << 11) + (d >> 2);
            if (dist == 0) break; // end of stream
            if (!match(dist + 0x4000, len + 2)) { *err = "lzo: bad match"; return false; }
            s = d & 3;
        }
        if (s && !literals(s)) return false;
        state = s;
    }
    if (op != outLen) { *err = "lzo: wrong decompressed size"; return false; }
    return true;
}

} // namespace lzo
} // namespace autosave
