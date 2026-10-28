#pragma once

#include <cstddef>
#include <cstdint>
#include "ControlIds.h"

// Portable firmware contract: no JUCE, OS types, packed structs, or host floats.
namespace mixsense::usbv1
{
constexpr std::uint8_t magic0 = 0xaa, magic1 = 0x55, version = 1;
constexpr std::size_t headerSize = 6, maxPayload = 6, maxFrame = headerSize + maxPayload;
constexpr std::uint8_t hapticEndpoint = 0, visualEndpoint = 0;
enum class Type : std::uint8_t
{
    ControlValue = 0x01, ButtonPressed = 0x02, ButtonReleased = 0x03,
    EncoderIncrement = 0x04, EncoderDecrement = 0x05,
    ControlSnapshot = 0x06,
    HapticLevel = 0x10, HapticPulse = 0x11, VisualLevel = 0x12
};
constexpr std::uint16_t read16 (const std::uint8_t* p) noexcept
{
    return static_cast<std::uint16_t> (p[0] | (static_cast<std::uint16_t> (p[1]) << 8));
}
constexpr std::uint32_t read32 (const std::uint8_t* p) noexcept
{
    return static_cast<std::uint32_t> (p[0]) | (static_cast<std::uint32_t> (p[1]) << 8)
         | (static_cast<std::uint32_t> (p[2]) << 16) | (static_cast<std::uint32_t> (p[3]) << 24);
}
inline void write16 (std::uint8_t* p, std::uint16_t v) noexcept
{
    p[0] = static_cast<std::uint8_t> (v); p[1] = static_cast<std::uint8_t> (v >> 8);
}
inline void write32 (std::uint8_t* p, std::uint32_t v) noexcept
{
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t> (v >> (8 * i));
}
}
