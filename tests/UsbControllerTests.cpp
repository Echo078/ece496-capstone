#include "PluginProcessor.h"
#include "SerialControllerTransport.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
using namespace mixsense;
using namespace std::chrono_literals;
void require (bool value, const char* description)
{
    if (! value) throw std::runtime_error (description);
}
bool near (float a, float b) { return std::abs (a - b) < 0.001f; }
template <typename Predicate> void waitFor (Predicate ready, const char* description, bool pumpTimers = false)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (! ready())
    {
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error (description);
        if (pumpTimers) juce::Timer::callPendingTimersSynchronously();
        std::this_thread::sleep_for (1ms);
    }
}
std::vector<std::uint8_t> bytesFor (TransportMessage message, FrameDirection direction)
{
    const auto frame = UsbFrameCodec::encode (message, direction);
    require (frame.has_value(), "encode fixture");
    return { frame->bytes.begin(), frame->bytes.begin() + static_cast<std::ptrdiff_t> (frame->size) };
}
constexpr auto input = FrameDirection::DeviceToHost;
constexpr auto output = FrameDirection::HostToDevice;

void protocolTests()
{
    const auto knob = bytesFor ({ MessageKind::Value, 0, 0.25f }, input);
    require (knob == std::vector<std::uint8_t> { 0xaa, 0x55, 1, 1, 0, 2, 0, 0x40 }, "documented knob hex");
    const auto feedback = bytesFor ({ MessageKind::Level, 0, 0.5f }, output);
    require (feedback == std::vector<std::uint8_t> { 0xaa, 0x55, 1, 0x10, 0, 2, 0, 0x80 }, "documented feedback hex");
    for (auto kind : { MessageKind::Value, MessageKind::Snapshot, MessageKind::Press, MessageKind::Release, MessageKind::Increment, MessageKind::Decrement })
    {
        const auto frame = bytesFor ({ kind, 5, 0.25f }, input);
        const auto decoded = UsbFrameCodec::decode (frame.data(), frame.size(), input);
        require (decoded && decoded->kind == kind && decoded->id == 5, "input roundtrip");
        if (kind == MessageKind::Value) require (near (decoded->value, 0.25f), "uint16 normalized conversion");
    }
    for (auto message : { TransportMessage { MessageKind::Level, 0, 1.0f },
                          TransportMessage { MessageKind::Level, 0, 0.25f, FeedbackTarget::Visual },
                          TransportMessage { MessageKind::Pulse, 0, 0.75f, FeedbackTarget::Haptic, 0x12345678 } })
    {
        const auto frame = bytesFor (message, output);
        const auto decoded = UsbFrameCodec::decode (frame.data(), frame.size(), output);
        require (decoded && decoded->kind == message.kind && decoded->target == message.target
                 && decoded->durationMs == message.durationMs && near (decoded->value, message.value), "feedback roundtrip");
        if (message.kind == MessageKind::Pulse)
            require (frame[8] == 0x78 && frame[11] == 0x12, "pulse duration little endian");
    }
    require (! UsbFrameCodec::encode ({ MessageKind::Level, 1, 0.5f }, output), "unknown feedback endpoint");
    require (! UsbFrameCodec::encode ({ MessageKind::Value, 6, 0.5f }, input), "unknown control");
    require (! UsbFrameCodec::encode ({ MessageKind::Value, 256, 0.5f }, input), "wide ID does not truncate");
    require (! UsbFrameCodec::encode ({ MessageKind::Value, 0, std::numeric_limits<float>::quiet_NaN() }, input), "nonfinite encode rejected");
    require (! UsbFrameCodec::decode (feedback.data(), feedback.size(), input), "wrong direction rejected");
    auto zeroPulse = bytesFor ({ MessageKind::Pulse, 0, 0.5f, FeedbackTarget::Haptic, 1 }, output);
    std::fill (zeroPulse.begin() + 8, zeroPulse.end(), std::uint8_t { 0 });
    require (! UsbFrameCodec::decode (zeroPulse.data(), zeroPulse.size(), output), "zero pulse payload rejected");
    zeroPulse.insert (zeroPulse.end(), feedback.begin(), feedback.end());
    UsbStreamParser feedbackParser (output);
    std::size_t validFeedback = 0;
    feedbackParser.feed (zeroPulse.data(), zeroPulse.size(), [&] (const auto& message)
    {
        require (message.kind == MessageKind::Level, "malformed pulse not emitted");
        ++validFeedback;
        return true;
    });
    require (validFeedback == 1, "parser recovers after malformed payload");

    // CDC reads may stop at any byte boundary, including within the header/value.
    for (std::size_t split = 0; split <= knob.size(); ++split)
    {
        UsbStreamParser parser (input);
        std::vector<TransportMessage> received;
        const auto sink = [&] (const auto& message) { received.push_back (message); return true; };
        parser.feed (knob.data(), split, sink);
        require (received.size() == (split == knob.size() ? 1u : 0u), "partial frame waits");
        parser.feed (knob.data() + split, knob.size() - split, sink);
        require (received.size() == 1 && near (received[0].value, 0.25f), "split reassembled exactly once");
    }
    // Malformed candidates must not consume an arbitrary advertised payload.
    for (std::size_t field : { 0u, 1u, 2u, 3u, 4u, 5u })
    {
        auto corrupt = knob;
        corrupt[field] = 0xff; // magic/version/type/control/length
        corrupt.insert (corrupt.end(), knob.begin(), knob.end());
        UsbStreamParser parser (input);
        std::size_t count = 0;
        parser.feed (corrupt.data(), corrupt.size(), [&] (const auto&) { ++count; return true; });
        require (count == 1, "recovery after malformed header");
    }
    std::vector<std::uint8_t> batch (4096, 0x13);
    batch.push_back (0xaa); // overlap: aa aa 55 must still synchronize
    for (int i = 0; i < 100; ++i) batch.insert (batch.end(), knob.begin(), knob.end());
    UsbStreamParser parser (input);
    std::size_t count = 0;
    parser.feed (batch.data(), batch.size(), [&] (const auto&) { ++count; return true; });
    require (count == 100 && parser.bufferedBytes() == 0, "noise plus multiple frames in one read");
    // Adversarial bytes exercise the fixed-size parser before a known-good frame.
    for (int i = 0; i < 10000; ++i)
    {
        const auto byte = static_cast<std::uint8_t> ((i * 73) & 255);
        parser.feed (&byte, 1, [] (const auto&) { return true; });
        require (parser.bufferedBytes() <= usbv1::maxFrame, "bounded parser storage");
    }
    parser.reset();
    require (! parser.feed (knob.data(), knob.size(), [] (const auto&) { return false; }), "sink overflow aborts parse");
}

void snapshotAndTruncationTests()
{
    const auto snapshot = bytesFor ({ MessageKind::Snapshot, 0, 0.25f }, input);
    require (snapshot == std::vector<std::uint8_t> { 0xaa, 0x55, 1, 6, 0, 2, 0, 0x40 }, "snapshot additive wire type");
    const auto snapshotMessage = UsbFrameCodec::decode (snapshot.data(), snapshot.size(), input);
    const auto snapshotEvent = LogicalMessageCodec::decode (*snapshotMessage);
    require (snapshotEvent && std::holds_alternative<InitialControlState> (snapshotEvent->change), "snapshot distinct from movement through logical codec");
    const auto movement = bytesFor ({ MessageKind::Value, 0, 0.75f }, input);
    UsbStreamParser parser (input);
    std::vector<TransportMessage> events;
    const auto sink = [&] (const auto& message) { events.push_back (message); return true; };
    parser.feed (movement.data(), usbv1::headerSize, sink); // Plausible header, missing value.
    parser.feed (movement.data(), movement.size(), sink);
    // With no checksum/escaping/timeout, AA 55 from the following frame is a
    // valid uint16 payload. Do not pretend this existing v1 ambiguity is solved.
    require (events.size() == 1 && ! near (events[0].value, 0.75f), "document plausible truncation can consume next frame magic");
    parser.feed (movement.data(), movement.size(), sink);
    require (events.size() == 2 && near (events.back().value, 0.75f), "subsequent complete frame resynchronizes");
    parser.reset(); events.clear();
    parser.feed (snapshot.data(), 4, sink);
    parser.reset(); // A new CDC session must never inherit this incomplete header.
    parser.feed (movement.data(), movement.size(), sink);
    require (events.size() == 1 && events[0].kind == MessageKind::Value, "session parser reset discards partial snapshot");
}

struct SimulatedLink
{
    std::mutex mutex;
    std::condition_variable gate;
    std::deque<std::uint8_t> inputBytes;
    std::vector<std::uint8_t> outputBytes;
    bool failOpen = false, failRead = false, failWrite = false;
    std::size_t failAfterWritten = std::numeric_limits<std::size_t>::max();
    bool holdRead = false, readEntered = false, isOpen = false;
    std::size_t readChunk = 1, writeChunk = 2;
    int opens = 0, closes = 0;
    std::string openedPort;
    std::thread::id owner = std::this_thread::get_id();
    bool ownerDidIO = false;
    std::thread::id ioThread;
    bool multipleIOThreads = false;
    void inject (const std::vector<std::uint8_t>& bytes)
    {
        std::lock_guard<std::mutex> lock (mutex);
        inputBytes.insert (inputBytes.end(), bytes.begin(), bytes.end());
    }
    void recordThread()
    {
        const auto caller = std::this_thread::get_id();
        ownerDidIO = ownerDidIO || caller == owner;
        if (ioThread == std::thread::id {}) ioThread = caller;
        multipleIOThreads = multipleIOThreads || caller != ioThread;
    }
};

// Real SerialControllerTransport worker/parser/queues, with only OS I/O replaced.
class SimulatedByteStream final : public ISerialByteStream
{
public:
    explicit SimulatedByteStream (std::shared_ptr<SimulatedLink> link) : link (std::move (link)) {}
    bool open (const std::string& port) override
    {
        std::lock_guard<std::mutex> lock (link->mutex);
        link->recordThread(); ++link->opens;
        link->openedPort = port;
        link->isOpen = ! link->failOpen;
        return link->isOpen;
    }
    void close() noexcept override
    {
        std::lock_guard<std::mutex> lock (link->mutex);
        link->recordThread();
        if (link->isOpen) { ++link->closes; link->isOpen = false; }
    }
    bool read (std::uint8_t* destination, std::size_t capacity, std::size_t& count) override
    {
        std::unique_lock<std::mutex> lock (link->mutex);
        link->recordThread(); link->readEntered = true;
        link->gate.wait (lock, [&] { return ! link->holdRead; });
        count = 0;
        if (link->failRead) return false;
        while (count < capacity && count < link->readChunk && ! link->inputBytes.empty())
        { destination[count++] = link->inputBytes.front(); link->inputBytes.pop_front(); }
        return true;
    }
    bool write (const std::uint8_t* bytes, std::size_t size, std::size_t& count) override
    {
        std::lock_guard<std::mutex> lock (link->mutex);
        link->recordThread(); count = 0;
        if (link->failWrite || link->outputBytes.size() >= link->failAfterWritten) return false;
        count = std::min (size, link->writeChunk);
        link->outputBytes.insert (link->outputBytes.end(), bytes, bytes + count);
        return true;
    }
private:
    std::shared_ptr<SimulatedLink> link;
};

// Always unblock a deliberately stalled backend before transport destruction,
// including on assertion failure (the worker's destructor joins).
struct ReleaseReadGate
{
    std::shared_ptr<SimulatedLink> link;
    ~ReleaseReadGate()
    {
        std::lock_guard<std::mutex> lock (link->mutex);
        link->holdRead = false; link->gate.notify_all();
    }
};

void start (SerialControllerTransport& transport)
{
    require (transport.setPort ("SIMULATED"), "set simulated port");
    require (transport.connect(), "async open accepted");
    waitFor ([&] { return transport.isConnected(); }, "simulated connect timeout");
}

void transportTests()
{
    auto link = std::make_shared<SimulatedLink>();
    {
        SerialControllerTransport transport (std::make_unique<SimulatedByteStream> (link));
        start (transport);
        link->inject (bytesFor ({ MessageKind::Press, 5 }, input));
        link->inject (bytesFor ({ MessageKind::Release, 5 }, input));
        link->inject (bytesFor ({ MessageKind::Increment, 0 }, input));
        std::vector<MessageKind> events;
        waitFor ([&]
        {
            if (auto event = transport.receive()) events.push_back (event->kind);
            return events.size() == 3;
        }, "receive fragmented discrete messages");
        require (events == std::vector<MessageKind> { MessageKind::Press, MessageKind::Release, MessageKind::Increment }, "serial discrete FIFO");
        waitFor ([&] { return transport.send ({ MessageKind::Pulse, 0, 0.5f, FeedbackTarget::Haptic, 100 }); }, "pulse enqueue");
        waitFor ([&] { std::lock_guard<std::mutex> lock (link->mutex); return link->outputBytes.size() == 12; }, "partial writes completed");
        {
            std::lock_guard<std::mutex> lock (link->mutex);
            const auto message = UsbFrameCodec::decode (link->outputBytes.data(), link->outputBytes.size(), output);
            require (message && message->kind == MessageKind::Pulse && message->durationMs == 100, "pulse arrives as full frame");
            link->failRead = true;
        }
        waitFor ([&] { return transport.connectionState() == ControllerConnectionState::Error; }, "read failure detected");
        require (transport.connectionError() == ControllerConnectionError::ReadFailed, "read failure reported");
        require (! transport.send ({ MessageKind::Level, 0, 1.0f }), "lost device rejects output");
        waitFor ([&] { std::lock_guard<std::mutex> lock (link->mutex); return link->closes == 1; }, "error closes resource");
        {
            std::lock_guard<std::mutex> lock (link->mutex);
            link->failRead = false; link->failWrite = true;
        }
        require (transport.connect(), "reconnect after read failure");
        waitFor ([&] { return transport.isConnected(); }, "reconnected");
        waitFor ([&] { return transport.send ({ MessageKind::Level, 0, 0.25f }); }, "write failure fixture enqueued");
        waitFor ([&] { return transport.connectionState() == ControllerConnectionState::Error; }, "write failure detected");
        require (transport.connectionError() == ControllerConnectionError::WriteFailed, "write failure reported");
        transport.disconnect();
        require (transport.connectionState() == ControllerConnectionState::Disconnected, "explicit disconnected state");
    }
    std::lock_guard<std::mutex> lock (link->mutex);
    require (! link->isOpen && link->closes == 2 && ! link->ownerDidIO && ! link->multipleIOThreads, "RAII cleanup and all I/O on one worker");
}

void openAndPartialWriteFailureTests()
{
    auto link = std::make_shared<SimulatedLink>();
    link->failOpen = true;
    SerialControllerTransport transport (std::make_unique<SimulatedByteStream> (link));
    require (transport.setPort ("SIMULATED") && transport.connect(), "failed open request accepted");
    waitFor ([&] { return transport.connectionState() == ControllerConnectionState::Error; }, "async open failure");
    require (transport.connectionError() == ControllerConnectionError::OpenFailed, "open failure reported");
    {
        std::lock_guard<std::mutex> lock (link->mutex);
        link->failOpen = false;
        link->failAfterWritten = 4;
    }
    require (transport.connect(), "retry failed open");
    waitFor ([&] { return transport.isConnected(); }, "retry open succeeds");
    waitFor ([&] { return transport.send ({ MessageKind::Pulse, 0, 1.0f, FeedbackTarget::Haptic, 20 }); }, "queue pulse before partial failure");
    waitFor ([&] { return transport.connectionState() == ControllerConnectionState::Error; }, "partial write failure ends session");
    require (transport.connectionError() == ControllerConnectionError::WriteFailed, "partial write failure reported");
    {
        std::lock_guard<std::mutex> lock (link->mutex);
        require (link->outputBytes.size() == 4, "partial pulse bytes are not retried");
        link->outputBytes.clear(); // Represents a device parser reset for a new session.
        link->failAfterWritten = std::numeric_limits<std::size_t>::max();
    }
    require (transport.connect(), "reconnect after partial write");
    waitFor ([&] { return transport.isConnected(); }, "open after partial write");
    waitFor ([&] { return transport.send ({ MessageKind::Level, 0, 0.0f }); }, "send only new session level");
    waitFor ([&] { std::lock_guard<std::mutex> lock (link->mutex); return link->outputBytes.size() >= 8; }, "new frame delivered");
    std::lock_guard<std::mutex> lock (link->mutex);
    require (link->outputBytes.size() == 8 && link->outputBytes[3] == 0x10, "partially written old pulse not replayed");
}

void boundedQueueAndNonblockingTests()
{
    auto link = std::make_shared<SimulatedLink>();
    link->holdRead = true;
    SerialControllerTransport transport (std::make_unique<SimulatedByteStream> (link));
    ReleaseReadGate release { link };
    start (transport);
    waitFor ([&] { std::lock_guard<std::mutex> lock (link->mutex); return link->readEntered; }, "backend entered blocking read");
    const auto startTime = std::chrono::steady_clock::now();
    require (! transport.receive(), "receive returns while worker I/O stalled");
    require (transport.send ({ MessageKind::Level, 0, 0.25f }), "send queues while worker stalled");
    require (transport.send ({ MessageKind::Level, 0, 0.75f }), "transport coalesces newer level");
    for (std::size_t i = 0; i < SerialControllerTransport::queueCapacity - 1; ++i)
        require (transport.send ({ MessageKind::Pulse, 0, 0.5f, FeedbackTarget::Haptic, static_cast<std::uint32_t> (i + 1) }), "discrete queue accepts within capacity");
    require (! transport.send ({ MessageKind::Pulse, 0, 0.5f, FeedbackTarget::Haptic, 999 }), "full discrete queue reports backpressure");
    require (transport.send ({ MessageKind::Level, 0, 1.0f }), "latest level replaces even in full queue");
    require (std::chrono::steady_clock::now() - startTime < 500ms, "owner APIs independent of stalled I/O");
    {
        std::lock_guard<std::mutex> lock (link->mutex);
        link->holdRead = false; link->gate.notify_all();
    }
    const auto expectedBytes = 8 + 12 * (SerialControllerTransport::queueCapacity - 1);
    waitFor ([&] { std::lock_guard<std::mutex> lock (link->mutex); return link->outputBytes.size() == expectedBytes; }, "queued frames transmitted");
    {
        std::lock_guard<std::mutex> lock (link->mutex);
        UsbStreamParser deviceParser (output);
        std::vector<TransportMessage> messages;
        deviceParser.feed (link->outputBytes.data(), link->outputBytes.size(), [&] (const auto& message) { messages.push_back (message); return true; });
        require (messages.size() == SerialControllerTransport::queueCapacity && near (messages[0].value, 1.0f), "transport latest state sent");
        for (std::size_t i = 1; i < messages.size(); ++i)
            require (messages[i].durationMs == i, "transport pulses all retained in order");
        link->holdRead = true; link->readEntered = false;
    }
    waitFor ([&] { std::lock_guard<std::mutex> lock (link->mutex); return link->readEntered; }, "worker stalled again");
    require (transport.send ({ MessageKind::Pulse, 0, 1.0f, FeedbackTarget::Haptic, 888 }), "stale pulse queued");
    transport.disconnect(); // Must return without joining the blocked worker.
    require (! transport.isConnected(), "disconnect immediate while I/O stalled");
    {
        std::lock_guard<std::mutex> lock (link->mutex);
        link->holdRead = false; link->gate.notify_all();
    }
    require (transport.connect(), "new session requested");
    waitFor ([&] { return transport.isConnected(); }, "new session opened");
    waitFor ([&] { return transport.send ({ MessageKind::Level, 0, 0.0f }); }, "new session level");
    waitFor ([&] { std::lock_guard<std::mutex> lock (link->mutex); return link->outputBytes.size() >= expectedBytes + 8; }, "new session output");
    {
        std::lock_guard<std::mutex> lock (link->mutex);
        require (link->outputBytes.size() == expectedBytes + 8, "old pulse never replayed after reconnect");
    }
}

void receiveOverflowTests()
{
    auto link = std::make_shared<SimulatedLink>();
    link->readChunk = 256;
    SerialControllerTransport transport (std::make_unique<SimulatedByteStream> (link));
    start (transport);
    for (int i = 0; i < 300; ++i) link->inject (bytesFor ({ MessageKind::Value, 0, static_cast<float> (i) / 300.0f }, input));
    link->inject (bytesFor ({ MessageKind::Press, 5 }, input));
    std::vector<TransportMessage> messages;
    waitFor ([&]
    {
        if (auto message = transport.receive()) messages.push_back (*message);
        return ! messages.empty() && messages.back().kind == MessageKind::Press;
    }, "continuous flood does not hide discrete press");
    // With no consumer, discrete overflow ends/reports the session instead of
    // silently overwriting a press or increment. Reconnect is explicitly required.
    for (std::size_t i = 0; i <= SerialControllerTransport::queueCapacity; ++i)
        link->inject (bytesFor ({ MessageKind::Increment, 0 }, input));
    waitFor ([&] { return transport.connectionState() == ControllerConnectionState::Error; }, "discrete overflow reported");
    require (transport.connectionError() == ControllerConnectionError::ReceiveOverflow, "explicit overflow reason");
}

void sessionReadAndParserResetTests()
{
    auto link = std::make_shared<SimulatedLink>();
    link->readChunk = 256;
    SerialControllerTransport transport (std::make_unique<SimulatedByteStream> (link));
    ReleaseReadGate release { link };
    start (transport);
    const auto oldFrame = bytesFor ({ MessageKind::Value, 0, 0.1f }, input);
    link->inject ({ oldFrame.begin(), oldFrame.begin() + 4 });
    waitFor ([&] { std::lock_guard<std::mutex> lock (link->mutex); return link->inputBytes.empty(); }, "partial old-session header consumed");
    {
        std::lock_guard<std::mutex> lock (link->mutex);
        link->holdRead = true; link->readEntered = false;
    }
    waitFor ([&] { std::lock_guard<std::mutex> lock (link->mutex); return link->readEntered; }, "old read held across session change");
    link->inject (oldFrame);
    transport.disconnect();
    require (transport.connect(), "request new session while old read in flight");
    {
        std::lock_guard<std::mutex> lock (link->mutex);
        link->holdRead = false; link->gate.notify_all();
    }
    waitFor ([&] { return transport.isConnected(); }, "new session opens after old read returns");
    require (! transport.receive(), "old-session read cannot enter new input queue");
    link->inject (bytesFor ({ MessageKind::Value, 0, 0.75f }, input));
    std::optional<TransportMessage> message;
    waitFor ([&] { message = transport.receive(); return message.has_value(); }, "new-session frame decoded");
    require (message->kind == MessageKind::Value && near (message->value, 0.75f), "new parser never combines old-session header with new input");
}

// Keep a dedicated audio thread processing throughout connection changes and
// message-thread servicing. It owns its buffer; assertions cross via atomics.
class RunningAudio
{
public:
    explicit RunningAudio (AudioPluginAudioProcessor& processor) : processor (processor)
    {
        processor.prepareToPlay (48000, 64);
        thread = std::thread ([this]
        {
            juce::AudioBuffer<float> audio (2, 64);
            juce::MidiBuffer midi;
            while (! stop.load())
            {
                for (int c = 0; c < 2; ++c)
                    // Below every allowed threshold. APVTS snapping can leave
                    // nominal 0 dB makeup a fraction of a micro-dB from zero.
                    for (int s = 0; s < 64; ++s) audio.setSample (c, s, 0.0001f);
                this->processor.processBlock (audio, midi);
                if (!(std::abs(audio.getSample(0, 0) - 0.0001f) < 1.0e-9f)
                    || !(std::abs(audio.getSample(1, 63) - 0.0001f) < 1.0e-9f)) failed = true;
                ++blocks;
                std::this_thread::yield();
            }
        });
    }
    ~RunningAudio() { stop = true; thread.join(); processor.releaseResources(); }
    std::atomic<std::size_t> blocks { 0 };
    std::atomic<bool> failed { false };
private:
    AudioPluginAudioProcessor& processor;
    std::atomic<bool> stop { false };
    std::thread thread;
};

void pluginTimerIntegrationTests()
{
    auto link = std::make_shared<SimulatedLink>();
    auto serial = std::make_unique<SerialControllerTransport> (std::make_unique<SimulatedByteStream> (link));
    auto* observedTransport = serial.get();
    AudioPluginAudioProcessor processor (std::move (serial));
    RunningAudio audio (processor);
    require (processor.connectController (" /dev/cu.usbmodemMiXeD "), "plugin async connect");
    waitFor ([&] { return observedTransport->isConnected(); }, "plugin connected");
    {
        std::lock_guard<std::mutex> lock (link->mutex);
        require (link->openedPort == "/dev/cu.usbmodemMiXeD"
                 && processor.getControllerPort() == "/dev/cu.usbmodemMiXeD", "application preserves case-sensitive serial path and trims whitespace");
    }
    // Startup reports are informational even when repeated before/after movement.
    auto& parameters = processor.getParameterState();
    parameters.getParameter ("threshold")->setValueNotifyingHost (0.8f);
    link->inject (bytesFor ({ MessageKind::Snapshot, 0, 0.1f }, input));
    link->inject (bytesFor ({ MessageKind::Press, 5 }, input)); // FIFO marker after snapshot.
    waitFor ([&] { return processor.getBypassParameter()->getValue() == 1.0f; }, "startup snapshot delivered", true);
    require (near (parameters.getParameter ("threshold")->getValue(), 0.8f), "serial snapshot never overwrites DAW threshold");
    parameters.getParameter ("bypass")->setValueNotifyingHost (0.0f);
    // Full byte stream -> real serial worker/parser -> processor timer -> bridge
    // -> APVTS. No test call directly invokes applyControllerEvent or service().
    link->inject (bytesFor ({ MessageKind::Value, 0, 0.25f }, input));
    link->inject (bytesFor ({ MessageKind::Press, 5 }, input));
    link->inject (bytesFor ({ MessageKind::Release, 5 }, input));
    waitFor ([&]
    {
        return near (parameters.getRawParameterValue ("threshold")->load(), -45.0f)
            && processor.getBypassParameter()->getValue() == 1.0f;
    }, "timer applies serial input to threshold/bypass", true);
    require (processor.submitControllerFeedback ({ { FeedbackTarget::Haptic, 0 }, FeedbackLevel { 0.5f } }), "application synthetic feedback accepted");
    waitFor ([&] { std::lock_guard<std::mutex> lock (link->mutex); return link->outputBytes.size() == 8; }, "timer sends synthetic feedback", true);
    {
        std::lock_guard<std::mutex> lock (link->mutex);
        const auto message = UsbFrameCodec::decode (link->outputBytes.data(), link->outputBytes.size(), output);
        require (message && near (message->value, 0.5f), "simulated device decodes host synthetic level");
        link->failRead = true;
    }
    waitFor ([&] { return processor.controllerConnectionState() == ControllerConnectionState::Error; }, "plugin observes link loss", true);
    require (! processor.submitControllerFeedback ({ { FeedbackTarget::Haptic, 0 }, FeedbackLevel { 1.0f } }), "plugin rejects feedback after loss");
    require (near (parameters.getRawParameterValue ("threshold")->load(), -45.0f), "loss preserves APVTS");
    const auto blocksAtLoss = audio.blocks.load();
    waitFor ([&] { return audio.blocks.load() > blocksAtLoss + 100; }, "audio continues after serial loss");
    {
        std::lock_guard<std::mutex> lock (link->mutex);
        require (! link->ownerDidIO && ! link->multipleIOThreads, "only device worker performs I/O, never message/audio thread");
        link->failRead = false;
    }
    require (processor.connectController ("SIMULATED"), "plugin reconnect");
    waitFor ([&] { return observedTransport->isConnected(); }, "plugin reconnect completes");
    link->inject (bytesFor ({ MessageKind::Snapshot, 0, 0.9f }, input));
    link->inject (bytesFor ({ MessageKind::Press, 5 }, input));
    waitFor ([&] { return processor.getBypassParameter()->getValue() == 0.0f; }, "reconnect snapshot delivered", true);
    require (near (parameters.getRawParameterValue ("threshold")->load(), -45.0f), "reconnect snapshot preserves APVTS");
    for (int i = 0; i < 200; ++i)
        link->inject (bytesFor ({ MessageKind::Value, 0, 0.75f }, input));
    waitFor ([&] { return near (parameters.getRawParameterValue ("threshold")->load(), -15.0f); }, "input flood after reconnect updates threshold", true);
    require (! audio.failed.load() && audio.blocks.load() > 100, "below-threshold audio stays intact through traffic/loss/reconnect");
    processor.disconnectController();
    std::cout << "CDC simulated bytes -> serial worker -> plugin timer -> threshold -45 dB/bypass; feedback -> device; audio survives loss\n";
}

void unavailableDeviceTest()
{
    AudioPluginAudioProcessor processor;
    RunningAudio audio (processor);
    require (processor.connectController ("not-a-serial-device"), "unavailable open requested");
    waitFor ([&] { return processor.controllerConnectionState() == ControllerConnectionState::Error; }, "native unavailable port fails gracefully");
    require (processor.controllerConnectionError() == ControllerConnectionError::OpenFailed, "unavailable port reported");
    waitFor ([&] { return audio.blocks.load() > 100; }, "audio with unavailable device");
    require (! audio.failed.load(), "unavailable device does not change audio");
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    try
    {
        protocolTests();
        snapshotAndTruncationTests();
        transportTests();
        openAndPartialWriteFailureTests();
        boundedQueueAndNonblockingTests();
        receiveOverflowTests();
        sessionReadAndParserResetTests();
        pluginTimerIntegrationTests();
        unavailableDeviceTest();
        std::cout << "CDC protocol, bounded worker transport, and plugin timer integration tests passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
