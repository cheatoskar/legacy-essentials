#pragma once
// LZO1X decompressor (GBX bodies), every access bounds-checked.
#include <stddef.h>
#include <stdint.h>

namespace autosave {
namespace lzo {

// Decompresses src into exactly outLen bytes of out. false (and *err) on any error or another size.
bool decompress(const uint8_t* src, size_t srcLen, uint8_t* out, size_t outLen, const char** err);

} // namespace lzo
} // namespace autosave
