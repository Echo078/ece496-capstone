#include "SerialControllerTransport.h"
#include <algorithm>
#include <chrono>

namespace mixsense
{
SerialControllerTransport::SerialControllerTransport (std::unique_ptr<ISerialByteStream> backend)
    : stream (std::move (backend)), worker ([this] { run(); }) {}

SerialControllerTransport::~SerialControllerTransport()
{
    {
        std::lock_guard<std::mutex> lock (mutex);
        stopping = true; requested = false; ++generation;
    }
    wake.notify_one();
    // Teardown only: joins after the current worker operation returns. Native
    // writes have finite timeouts. Never destroy this object on the audio thread.
    worker.join();
}

bool SerialControllerTransport::setPort (std::string name)
{
    if (name.empty() || name.size() > 1024 || name.find ('\0') != std::string::npos) return false;
    std::lock_guard<std::mutex> lock (mutex);
    if (state.load() == ControllerConnectionState::Connecting || isConnected()) return false;
    port = std::move (name);
    return true;
}

bool SerialControllerTransport::connect()
{
    std::lock_guard<std::mutex> lock (mutex);
    if (stopping || port.empty()) return false;
    if (requested) return true;
    incoming.clear(); outgoing.clear(); ++generation;
    publishedSession = generation;
    requested = true;
    error = ControllerConnectionError::None;
    state = ControllerConnectionState::Connecting;
    wake.notify_one();
    return true;
}

void SerialControllerTransport::disconnect()
{
    std::lock_guard<std::mutex> lock (mutex);
    requested = false; ++generation;
    publishedSession = generation;
    incoming.clear(); outgoing.clear();
    state = ControllerConnectionState::Disconnected;
    error = ControllerConnectionError::None;
    wake.notify_one();
}

std::optional<TransportMessage> SerialControllerTransport::receive()
{
    std::unique_lock<std::mutex> lock (mutex, std::try_to_lock);
    if (! lock || ! isConnected() || incoming.empty()) return std::nullopt;
    auto message = incoming.front(); incoming.pop_front();
    return message;
}

bool SerialControllerTransport::send (const TransportMessage& message)
{
    if (! UsbFrameCodec::encode (message, FrameDirection::HostToDevice)) return false;
    std::unique_lock<std::mutex> lock (mutex, std::try_to_lock);
    if (! lock || ! isConnected()) return false;
    if (message.kind == MessageKind::Level)
    {
        auto previous = std::find_if (outgoing.begin(), outgoing.end(), [&] (const auto& item)
        { return item.kind == MessageKind::Level && item.target == message.target && item.id == message.id; });
        if (previous != outgoing.end()) { *previous = message; return true; }
    }
    if (outgoing.size() >= queueCapacity) return false;
    outgoing.push_back (message);
    wake.notify_one();
    return true;
}

bool SerialControllerTransport::currentSession (std::uint64_t session)
{
    std::lock_guard<std::mutex> lock (mutex);
    return ! stopping && requested && generation == session;
}

void SerialControllerTransport::fail (std::uint64_t session, ControllerConnectionError reason)
{
    std::lock_guard<std::mutex> lock (mutex);
    if (generation != session) return;
    requested = false;
    incoming.clear(); outgoing.clear();
    error = reason;
    state = ControllerConnectionState::Error;
}

bool SerialControllerTransport::enqueueInput (std::uint64_t session, const TransportMessage& message)
{
    std::lock_guard<std::mutex> lock (mutex);
    if (! requested || generation != session || stopping) return false;
    if (message.kind == MessageKind::Value || message.kind == MessageKind::Snapshot)
    {
        const auto previous = std::find_if (incoming.begin(), incoming.end(), [&] (const auto& item)
        { return item.kind == message.kind && item.id == message.id; });
        if (previous != incoming.end()) incoming.erase (previous);
        if (incoming.size() >= queueCapacity) { ++droppedInputs; return true; }
    }
    else if (incoming.size() >= queueCapacity)
    {
        // Never silently overwrite a discrete action. End/report this session so
        // the application knows delivery continuity was lost.
        requested = false; incoming.clear(); outgoing.clear();
        error = ControllerConnectionError::ReceiveOverflow;
        state = ControllerConnectionState::Error;
        return false;
    }
    incoming.push_back (message);
    return true;
}

void SerialControllerTransport::run()
{
    std::uint64_t session = 0;
    for (;;)
    {
        try
        {
            std::string selectedPort;
            {
                std::unique_lock<std::mutex> lock (mutex);
                wake.wait (lock, [&] { return stopping || requested; });
                if (stopping) break;
                session = generation; selectedPort = port;
            }
            if (! stream || ! stream->open (selectedPort))
            {
                if (stream) stream->close();
                fail (session, ControllerConnectionError::OpenFailed);
                continue;
            }
            {
                std::lock_guard<std::mutex> lock (mutex);
                if (requested && generation == session && ! stopping)
                    state = ControllerConnectionState::Connected;
            }
            UsbStreamParser parser (FrameDirection::DeviceToHost);
            while (currentSession (session))
            {
                std::array<std::uint8_t, 256> bytes {};
                std::size_t count = 0;
                if (! stream->read (bytes.data(), bytes.size(), count) || count > bytes.size())
                { fail (session, ControllerConnectionError::ReadFailed); break; }
                const auto beforeRejected = parser.rejectedCandidates();
                const bool accepted = parser.feed (bytes.data(), count, [&] (const auto& message)
                { return enqueueInput (session, message); });
                rejected.fetch_add (parser.rejectedCandidates() - beforeRejected);
                if (! accepted) break;

                std::optional<TransportMessage> message;
                {
                    std::lock_guard<std::mutex> lock (mutex);
                    if (generation == session && requested && ! outgoing.empty())
                    { message = outgoing.front(); outgoing.pop_front(); }
                }
                if (message)
                {
                    const auto frame = UsbFrameCodec::encode (*message, FrameDirection::HostToDevice);
                    if (! frame) { fail (session, ControllerConnectionError::WriteFailed); break; }
                    std::size_t offset = 0;
                    while (offset < frame->size && currentSession (session))
                    {
                        std::size_t written = 0;
                        if (! stream->write (frame->bytes.data() + offset, frame->size - offset, written)
                            || written == 0 || written > frame->size - offset)
                        { fail (session, ControllerConnectionError::WriteFailed); break; }
                        offset += written;
                    }
                }
                // Polling backoff is a device-worker scheduling choice, not a
                // haptic rate requirement. New output/disconnect wakes it early.
                if (count == 0)
                {
                    std::unique_lock<std::mutex> lock (mutex);
                    wake.wait_for (lock, std::chrono::milliseconds (2), [&]
                    { return stopping || ! requested || generation != session || ! outgoing.empty(); });
                }
            }
            stream->close();
        }
        catch (...)
        {
            fail (session, ControllerConnectionError::WorkerFailed);
            if (stream) stream->close();
        }
    }
    if (stream) stream->close();
}
}
