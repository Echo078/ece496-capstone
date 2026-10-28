#pragma once

#include "UsbFrameCodec.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mixsense
{
// Worker-only byte backend. read returns promptly (zero = idle), write may be
// partial, zero/false = failure. No backend method is called by the audio/UI path.
class ISerialByteStream
{
public:
    virtual ~ISerialByteStream() = default;
    virtual bool open (const std::string& port) = 0;
    virtual void close() noexcept = 0;
    virtual bool read (std::uint8_t* destination, std::size_t capacity, std::size_t& received) = 0;
    virtual bool write (const std::uint8_t* bytes, std::size_t size, std::size_t& written) = 0;
};
std::unique_ptr<ISerialByteStream> makeNativeSerialByteStream();

enum class ControllerConnectionState { Disconnected, Connecting, Connected, Error };
enum class ControllerConnectionError { None, OpenFailed, ReadFailed, WriteFailed, ReceiveOverflow, WorkerFailed };

class SerialControllerTransport final : public IControllerTransport
{
public:
    static constexpr std::size_t queueCapacity = 128;
    explicit SerialControllerTransport (std::unique_ptr<ISerialByteStream> = makeNativeSerialByteStream());
    ~SerialControllerTransport() override;
    SerialControllerTransport (const SerialControllerTransport&) = delete;
    SerialControllerTransport& operator= (const SerialControllerTransport&) = delete;

    // Owner/message thread only. Port changes require Disconnected or Error state.
    bool setPort (std::string);
    bool connect() override; // true = asynchronous open request accepted, NOT connected yet.
    void disconnect() override; // invalidates session/queues immediately; worker closes device.
    bool isConnected() const override { return state.load() == ControllerConnectionState::Connected; }
    ControllerConnectionState connectionState() const noexcept { return state.load(); }
    ControllerConnectionError connectionError() const noexcept { return error.load(); }
    std::uint64_t sessionId() const noexcept override { return publishedSession.load(); }
    std::size_t rejectedFrames() const noexcept { return rejected.load(); }
    std::size_t droppedContinuousInputs() const noexcept { return droppedInputs.load(); }
    std::optional<TransportMessage> receive() override;
    bool send (const TransportMessage&) override;
    bool supports (const TransportMessage& message) const override
    { return UsbFrameCodec::encode (message, FrameDirection::HostToDevice).has_value(); }
private:
    void run();
    void fail (std::uint64_t session, ControllerConnectionError);
    bool currentSession (std::uint64_t session);
    bool enqueueInput (std::uint64_t session, const TransportMessage&);

    std::unique_ptr<ISerialByteStream> stream;
    std::atomic<ControllerConnectionState> state { ControllerConnectionState::Disconnected };
    std::atomic<ControllerConnectionError> error { ControllerConnectionError::None };
    std::atomic<std::size_t> rejected { 0 }, droppedInputs { 0 };
    std::atomic<std::uint64_t> publishedSession { 0 };
    // Mutex protects only bounded memory operations, never backend I/O or waiting.
    // send/receive use try_lock and report backpressure/idle rather than wait.
    std::mutex mutex;
    std::condition_variable wake;
    std::string port;
    bool stopping = false, requested = false;
    std::uint64_t generation = 0;
    std::deque<TransportMessage> incoming, outgoing;
    std::thread worker;
};
}
