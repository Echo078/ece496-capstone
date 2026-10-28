#include "PluginProcessor.h"
#include "ControllerBridge.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
void require (bool condition, const char* description)
{
    if (! condition) throw std::runtime_error (description);
}
bool near (float a, float b) { return std::abs (a - b) < 0.001f; }
using namespace mixsense;
constexpr auto thresholdId = static_cast<std::uint16_t> (ControlId::Threshold);
constexpr auto bypassId = static_cast<std::uint16_t> (ControlId::Bypass);
constexpr FeedbackAddress haptic { FeedbackTarget::Haptic, 0 };

// Milestone 1: fake knob -> transport -> middleware -> adapter -> JUCE parameter.
// main() initializes JUCE on this thread; no audio callback or device worker is used.
void fakeKnobUpdatesThresholdTest()
{
    AudioPluginAudioProcessor processor;
    InMemoryControllerTransport mockDevice;

    // This callback is the application boundary. The bridge knows logical controls;
    // the processor's adapter owns the control-to-parameter mapping and JUCE updates.
    std::size_t deliveredEvents = 0;
    ControllerBridge bridge (mockDevice, [&] (const ControllerEvent& event)
    {
        require (processor.applyControllerEvent (event), "milestone: knob event applied to JUCE");
        ++deliveredEvents;
    });
    require (bridge.connect(), "milestone: connect fake knob");

    auto& parameters = processor.getParameterState();
    const auto* thresholdParameterId = ControllerParameterAdapter::parameterId (ControlId::Threshold);
    const auto* thresholdDb = parameters.getRawParameterValue (thresholdParameterId);
    require (thresholdDb != nullptr, "milestone: threshold parameter exists");
    const auto beforeDb = thresholdDb->load();
    require (near (beforeDb, -24.0f), "milestone: initial threshold is -24 dB");

    // Simulate firmware reporting a knob at 25% of its logical range. Inject at
    // the transport boundary, rather than bypassing the middleware with apply().
    require (mockDevice.inject ({ MessageKind::Value, thresholdId, 0.25f }), "milestone: inject fake knob message");
    require (deliveredEvents == 0 && near (thresholdDb->load(), beforeDb), "milestone: input waits for service");

    // receive -> decode/validate -> ControllerEvent -> callback -> APVTS.
    bridge.service();
    require (deliveredEvents == 1, "milestone: bridge delivers one logical knob event");
    require (near (parameters.getParameter (thresholdParameterId)->getValue(), 0.25f), "milestone: normalized threshold is 0.25");
    require (near (thresholdDb->load(), -45.0f), "milestone: threshold changed to -45 dB");
    std::cout << "[milestone 1] Fake knob 0.25 -> middleware -> JUCE threshold: "
              << beforeDb << " dB -> " << thresholdDb->load() << " dB\n";
    bridge.disconnect();
}

// Milestone 2: synthetic application feedback cue -> middleware -> mock device.
void fakeGainReductionFeedbackReachesMockTest()
{
    InMemoryControllerTransport mockDevice;
    ControllerBridge bridge (mockDevice, {}); // This direction needs no input sink.
    require (bridge.connect(), "milestone: connect fake feedback device");

    // Stand in for an application-selected cue associated with gain reduction.
    // Its intensity is a fixed test fixture: no audio analysis or dB-to-haptic
    // mapping is implemented. The middleware receives only normalized output state.
    const FeedbackCommand fakeGainReductionCue { haptic, FeedbackLevel { 0.6f } };
    require (bridge.submitFeedback (fakeGainReductionCue), "milestone: feedback cue accepted");
    require (mockDevice.sentMessages().empty(), "milestone: acceptance queues feedback before send");

    // validate/coalesce -> encode -> transport.send(). The mock records exactly
    // what the future device link would be asked to deliver.
    bridge.service();
    const auto& received = mockDevice.sentMessages();
    require (received.size() == 1, "milestone: mock device receives one feedback command");
    const auto& command = received.front();
    require (command.kind == MessageKind::Level, "milestone: feedback encoded as level state");
    require (command.target == FeedbackTarget::Haptic && command.id == haptic.id, "milestone: feedback reaches correct logical endpoint");
    require (near (command.value, 0.6f) && command.durationMs == 0, "milestone: normalized continuous feedback preserved");
    std::cout << "[milestone 2] Fake gain-reduction cue 0.6 -> middleware -> mock haptic endpoint "
              << command.id << ": level " << command.value << '\n';
    bridge.disconnect();
}

// Supporting codec safeguards, independent of application parameter semantics.
void codecTests()
{
    auto event = LogicalMessageCodec::decode ({ MessageKind::Value, thresholdId, 0.25f });
    require (event && event->control == ControlId::Threshold, "decode logical control");
    require (near (std::get<ContinuousValue> (event->change).normalized, 0.25f), "decode value");
    require (! LogicalMessageCodec::decode ({ MessageKind::Value, 999, 0.5f }), "unknown control rejected");
    require (! LogicalMessageCodec::decode ({ static_cast<MessageKind> (999), thresholdId }), "unknown kind rejected");
    event = LogicalMessageCodec::decode ({ MessageKind::Value, thresholdId, 2.0f });
    require (event && std::get<ContinuousValue> (event->change).normalized == 1.0f, "upper clamp");
    event = LogicalMessageCodec::decode ({ MessageKind::Value, thresholdId, -1.0f });
    require (event && std::get<ContinuousValue> (event->change).normalized == 0.0f, "lower clamp");
    require (! LogicalMessageCodec::decode ({ MessageKind::Value, thresholdId, std::numeric_limits<float>::quiet_NaN() }), "NaN rejected");
    require (! LogicalMessageCodec::decode ({ MessageKind::Value, thresholdId, std::numeric_limits<float>::infinity() }), "infinity rejected");
}

// Supporting delivery rules: discrete FIFO, continuous coalescing, and timing.
void bridgeTests()
{
    InMemoryControllerTransport transport;
    std::vector<ControllerEvent> events;
    ControllerBridge bridge (transport, [&] (const auto& event) { events.push_back (event); });
    require (bridge.connect(), "connect mock");
    transport.inject ({ MessageKind::Press, bypassId });
    transport.inject ({ MessageKind::Release, bypassId });
    transport.inject ({ MessageKind::Increment, thresholdId });
    transport.inject ({ MessageKind::Increment, thresholdId });
    transport.inject ({ MessageKind::Decrement, thresholdId });
    const auto now = ControllerBridge::Clock::now();
    bridge.service (now, 2);
    require (events.size() == 2, "bounded receive service");
    bridge.service (now);
    require (events.size() == 5, "discrete inputs never coalesced");
    require (std::get<DiscreteAction> (events[0].change) == DiscreteAction::ButtonPressed
             && std::get<DiscreteAction> (events[1].change) == DiscreteAction::ButtonReleased
             && std::get<DiscreteAction> (events[4].change) == DiscreteAction::Decrement, "discrete FIFO order");
    require (! bridge.submitFeedback ({ { static_cast<FeedbackTarget> (99), 0 }, FeedbackLevel { 0.5f } }), "unknown feedback rejected");
    require (! bridge.submitFeedback ({ haptic, FeedbackLevel { std::numeric_limits<float>::infinity() } }), "invalid feedback rejected");
    require (! bridge.submitFeedback ({ haptic, FeedbackPulse { 0.5f, 0 } }), "zero duration rejected");
    require (bridge.submitFeedback ({ haptic, FeedbackLevel { 2.0f } }), "accept feedback");
    bridge.service (now);
    require (transport.sentMessages().size() == 1 && transport.sentMessages()[0].kind == MessageKind::Level
             && near (transport.sentMessages()[0].value, 1.0f), "encode/send clamped feedback");
    bridge.setFeedbackInterval (std::chrono::milliseconds (100));
    bridge.submitFeedback ({ haptic, FeedbackLevel { 0.4f } });
    bridge.submitFeedback ({ haptic, FeedbackLevel { 0.7f } });
    bridge.service (now + std::chrono::milliseconds (50));
    require (transport.sentMessages().size() == 1, "configured rate limit");
    bridge.submitFeedback ({ haptic, FeedbackPulse { 0.2f, 10 } });
    bridge.submitFeedback ({ haptic, FeedbackPulse { 0.3f, 20 } });
    bridge.service (now + std::chrono::milliseconds (60));
    require (transport.sentMessages().size() == 3 && transport.sentMessages()[1].durationMs == 10
             && transport.sentMessages()[2].durationMs == 20, "pulses retained FIFO independently of levels");
    bridge.service (now + std::chrono::milliseconds (100));
    require (transport.sentMessages().size() == 4 && near (transport.sentMessages().back().value, 0.7f), "latest feedback wins");
    bridge.submitFeedback ({ haptic, FeedbackPulse { 0.5f, 10 } });
    transport.disconnect();
    bridge.service (now);
    require (! bridge.submitFeedback ({ haptic, FeedbackLevel { 0.5f } }), "disconnected submit rejected");
    bridge.connect();
    bridge.service (now);
    require (transport.sentMessages().size() == 4, "lost session does not replay pending pulses");
    bridge.submitFeedback ({ haptic, FeedbackLevel { 0.2f } });
    bridge.submitFeedback ({ { FeedbackTarget::Visual, 0 }, FeedbackLevel { -0.5f } });
    bridge.service (now);
    require (transport.sentMessages().size() == 6
             && transport.sentMessages()[4].target == FeedbackTarget::Haptic
             && transport.sentMessages()[5].target == FeedbackTarget::Visual
             && transport.sentMessages()[5].value == 0.0f, "visual and haptic addresses independent with lower clamp");
}

class ObservedTransport final : public IControllerTransport
{
public:
    bool connect() override { ++calls; return mock.connect(); }
    void disconnect() override { ++calls; mock.disconnect(); }
    bool isConnected() const override { ++calls; return mock.isConnected(); }
    std::uint64_t sessionId() const noexcept override { return mock.sessionId(); }
    std::optional<TransportMessage> receive() override { ++calls; return mock.receive(); }
    bool send (const TransportMessage& message) override { ++calls; return acceptWrites && mock.send (message); }
    mutable int calls = 0;
    bool acceptWrites = true;
    InMemoryControllerTransport mock;
};

// Supporting connection rules: retry within a session, cancel on reconnect.
void retryTests()
{
    ObservedTransport transport;
    ControllerBridge bridge (transport, {});
    bridge.connect();
    transport.acceptWrites = false;
    bridge.submitFeedback ({ haptic, FeedbackPulse { 0.2f, 10 } });
    bridge.submitFeedback ({ haptic, FeedbackPulse { 0.3f, 20 } });
    bridge.submitFeedback ({ haptic, FeedbackLevel { 0.4f } });
    bridge.service();
    require (transport.mock.sentMessages().empty(), "failed writes not marked sent");
    bridge.submitFeedback ({ haptic, FeedbackLevel { 0.8f } });
    transport.acceptWrites = true;
    bridge.service();
    const auto& sent = transport.mock.sentMessages();
    require (sent.size() == 3 && sent[0].durationMs == 10 && sent[1].durationMs == 20
             && near (sent[2].value, 0.8f), "retry preserves pulses and newest state");
    bridge.submitFeedback ({ haptic, FeedbackPulse { 0.5f, 30 } });
    transport.disconnect();
    bridge.connect();
    bridge.service();
    require (sent.size() == 3, "immediate reconnect cancels old session output");
}

// Broader plugin checks: bypass, mappings, saved state, and disconnected audio.
void pluginTests()
{
    AudioPluginAudioProcessor processor;
    ObservedTransport transport;
    ControllerBridge bridge (transport, [&] (const auto& event)
    { require (processor.applyControllerEvent (event), "apply mapped event"); });
    bridge.connect();
    transport.mock.inject ({ MessageKind::Value, thresholdId, 0.25f });
    transport.mock.inject ({ MessageKind::Press, bypassId });
    transport.mock.inject ({ MessageKind::Release, bypassId });
    bridge.service();
    auto& state = processor.getParameterState();
    require (near (state.getRawParameterValue ("threshold")->load(), -45.0f), "threshold unit conversion");
    require (processor.getBypassParameter()->getValue() == 1.0f, "bypass toggle on press only");
    for (auto control : { ControlId::Threshold, ControlId::Ratio, ControlId::Attack, ControlId::Release, ControlId::MakeupGain, ControlId::Bypass })
        require (state.getParameter (ControllerParameterAdapter::parameterId (control)) != nullptr, "all controls mapped");
    require (! processor.applyControllerEvent ({ static_cast<ControlId> (999), ContinuousValue { 0.5f } }), "adapter unknown ID rejected");
    require (! processor.applyControllerEvent ({ ControlId::Threshold, ContinuousValue { std::numeric_limits<float>::quiet_NaN() } }), "adapter invalid value rejected");
    processor.applyControllerEvent ({ ControlId::Threshold, DiscreteAction::Increment });
    require (near (state.getParameter ("threshold")->getValue(), 0.26f), "encoder step applied");
    juce::MemoryBlock saved;
    processor.getStateInformation (saved);
    state.getParameter ("threshold")->setValueNotifyingHost (0.9f);
    processor.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    require (near (state.getParameter ("threshold")->getValue(), 0.26f), "APVTS state restored");
    bridge.disconnect();
    const int callsBeforeAudio = transport.calls;
    processor.prepareToPlay (48000, 64);
    juce::AudioBuffer<float> audio (2, 64);
    juce::MidiBuffer midi;
    for (int iteration = 0; iteration < 4; ++iteration)
    {
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < 64; ++sample) audio.setSample (channel, sample, 0.25f);
        processor.processBlock (audio, midi);
        require (audio.getSample (0, 0) == 0.25f && audio.getSample (1, 63) == 0.25f, "disconnected audio remains passthrough");
    }
    require (transport.calls == callsBeforeAudio, "audio path never calls transport");
    processor.releaseResources();
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    try
    {
        fakeKnobUpdatesThresholdTest();
        fakeGainReductionFeedbackReachesMockTest();
        codecTests();
        bridgeTests();
        retryTests();
        pluginTests();
        std::cout << "Controller codec, bridge, and plugin integration tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
