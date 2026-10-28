#include "UsbFrameCodec.h"
#include <algorithm>
#include <cmath>

namespace mixsense
{
namespace
{
int payloadLength (std::uint8_t type, FrameDirection direction)
{
    using usbv1::Type;
    if (direction == FrameDirection::DeviceToHost)
    {
        switch (static_cast<Type> (type))
        {
            case Type::ControlValue: case Type::ControlSnapshot: return 2;
            case Type::ButtonPressed: case Type::ButtonReleased:
            case Type::EncoderIncrement: case Type::EncoderDecrement: return 0;
            default: return -1;
        }
    }
    switch (static_cast<Type> (type))
    {
        case Type::HapticLevel: case Type::VisualLevel: return 2;
        case Type::HapticPulse: return 6;
        default: return -1;
    }
}
}

bool UsbFrameCodec::validHeader (const std::uint8_t* p, FrameDirection direction)
{
    if (p[0] != usbv1::magic0 || p[1] != usbv1::magic1 || p[2] != usbv1::version) return false;
    const int length = payloadLength (p[3], direction);
    if (length < 0 || p[5] != length || p[5] > usbv1::maxPayload) return false;
    if (direction == FrameDirection::DeviceToHost)
        return isKnownControl (static_cast<ControlId> (p[4]));
    return p[4] == 0; // v1 supports endpoint 0 in each feedback category.
}

std::optional<TransportMessage> UsbFrameCodec::decode (const std::uint8_t* p, std::size_t size, FrameDirection direction)
{
    if (size < usbv1::headerSize || ! validHeader (p, direction) || size != usbv1::headerSize + p[5])
        return std::nullopt;
    TransportMessage result { MessageKind::Value, p[4] };
    using usbv1::Type;
    switch (static_cast<Type> (p[3]))
    {
        case Type::ControlValue: result.kind = MessageKind::Value; break;
        case Type::ControlSnapshot: result.kind = MessageKind::Snapshot; break;
        case Type::ButtonPressed: result.kind = MessageKind::Press; break;
        case Type::ButtonReleased: result.kind = MessageKind::Release; break;
        case Type::EncoderIncrement: result.kind = MessageKind::Increment; break;
        case Type::EncoderDecrement: result.kind = MessageKind::Decrement; break;
        case Type::HapticLevel: result.kind = MessageKind::Level; break;
        case Type::VisualLevel: result.kind = MessageKind::Level; result.target = FeedbackTarget::Visual; break;
        case Type::HapticPulse:
            result.kind = MessageKind::Pulse;
            result.durationMs = usbv1::read32 (p + 8);
            if (result.durationMs == 0) return std::nullopt;
            break;
        default: return std::nullopt;
    }
    if (p[5] >= 2) result.value = static_cast<float> (usbv1::read16 (p + 6)) / 65535.0f;
    return result;
}

std::optional<UsbFrame> UsbFrameCodec::encode (const TransportMessage& message, FrameDirection direction)
{
    using usbv1::Type;
    Type type;
    switch (message.kind)
    {
        case MessageKind::Value: type = Type::ControlValue; break;
        case MessageKind::Snapshot: type = Type::ControlSnapshot; break;
        case MessageKind::Press: type = Type::ButtonPressed; break;
        case MessageKind::Release: type = Type::ButtonReleased; break;
        case MessageKind::Increment: type = Type::EncoderIncrement; break;
        case MessageKind::Decrement: type = Type::EncoderDecrement; break;
        case MessageKind::Level:
            if (message.target != FeedbackTarget::Haptic && message.target != FeedbackTarget::Visual) return std::nullopt;
            type = message.target == FeedbackTarget::Haptic ? Type::HapticLevel : Type::VisualLevel;
            break;
        case MessageKind::Pulse:
            if (message.target != FeedbackTarget::Haptic || message.durationMs == 0) return std::nullopt;
            type = Type::HapticPulse; break;
        default: return std::nullopt;
    }
    const int length = payloadLength (static_cast<std::uint8_t> (type), direction);
    if (length < 0 || message.id > 255) return std::nullopt;
    UsbFrame frame;
    frame.bytes[0] = usbv1::magic0; frame.bytes[1] = usbv1::magic1;
    frame.bytes[2] = usbv1::version; frame.bytes[3] = static_cast<std::uint8_t> (type);
    frame.bytes[4] = static_cast<std::uint8_t> (message.id); frame.bytes[5] = static_cast<std::uint8_t> (length);
    frame.size = usbv1::headerSize + static_cast<std::size_t> (length);
    if (! validHeader (frame.bytes.data(), direction)) return std::nullopt;
    if (length >= 2)
    {
        if (! std::isfinite (message.value)) return std::nullopt;
        usbv1::write16 (frame.bytes.data() + 6, static_cast<std::uint16_t> (std::lround (std::clamp (message.value, 0.0f, 1.0f) * 65535.0f)));
    }
    if (type == Type::HapticPulse) usbv1::write32 (frame.bytes.data() + 8, message.durationMs);
    return frame;
}

void UsbStreamParser::discardFirst()
{
    std::move (buffer.begin() + 1, buffer.begin() + static_cast<std::ptrdiff_t> (used), buffer.begin());
    --used;
}

bool UsbStreamParser::feed (const std::uint8_t* bytes, std::size_t size,
                          const std::function<bool (const TransportMessage&)>& sink)
{
    for (std::size_t i = 0; i < size; ++i)
    {
        buffer[used++] = bytes[i];
        while (used != 0)
        {
            if (buffer[0] != usbv1::magic0 || (used >= 2 && buffer[1] != usbv1::magic1))
            { discardFirst(); continue; }
            if (used < usbv1::headerSize) break;
            if (! UsbFrameCodec::validHeader (buffer.data(), direction))
            { ++rejected; discardFirst(); continue; }
            const auto frameSize = usbv1::headerSize + buffer[5];
            if (used < frameSize) break;
            if (auto message = UsbFrameCodec::decode (buffer.data(), frameSize, direction))
            {
                std::move (buffer.begin() + static_cast<std::ptrdiff_t> (frameSize),
                           buffer.begin() + static_cast<std::ptrdiff_t> (used), buffer.begin());
                used -= frameSize;
                if (! sink (*message)) return false;
                continue;
            }
            ++rejected;
            discardFirst();
        }
    }
    return true;
}
}
