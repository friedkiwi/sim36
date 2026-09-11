// Big-endian field access on guest byte views.  Every guest structure is a
// span of bytes read and written through these helpers; nothing overlays a
// host struct on guest memory.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sim36::storage {

inline uint16_t be16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline uint32_t be24(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
}

inline uint32_t be32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

inline void putBe16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

inline void putBe24(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v >> 16);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v);
}

inline void putBe32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

// Copies `n` bytes out of a byte view into a host value (memcpy, never a cast).
template <typename T>
inline T copyOut(const uint8_t* p)
{
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

}  // namespace sim36::storage
