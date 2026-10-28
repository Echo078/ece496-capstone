#pragma once

#include "ControllerTransport.h"
#include "UsbProtocolV1.h"
#include <array>
#include <functional>

namespace mixsense
{
enum class FrameDirection { DeviceToHost, HostToDevice };
struct UsbFrame
{
    std::array<std::uint8_t, usbv1::maxFrame> bytes {};
    std::size_t size = 0;
};
class UsbFrameCodec
{
public:
    static std::optional<UsbFrame> encode (const TransportMessage&, FrameDirection);
    static std::optional<TransportMessage> decode (const std::uint8_t*, std::size_t, FrameDirection);
    static bool validHeader (const std::uint8_t*, FrameDirection);
};

// Exactly one maximum-sized candidate retained, regardless of input chunk size.
// Sink returning false aborts the chunk (e.g. queue overflow ends the session).
class UsbStreamParser
{
public:
    explicit UsbStreamParser (FrameDirection direction) : direction (direction) {}
    bool feed (const std::uint8_t*, std::size_t, const std::function<bool (const TransportMessage&)>&);
    void reset() noexcept { used = 0; }
    std::size_t bufferedBytes() const noexcept { return used; }
    std::size_t rejectedCandidates() const noexcept { return rejected; }
private:
    void discardFirst();
    FrameDirection direction;
    std::array<std::uint8_t, usbv1::maxFrame> buffer {};
    std::size_t used = 0, rejected = 0;
};
}
