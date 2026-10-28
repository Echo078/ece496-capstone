#include "PluginProcessor.h"
#include "ContinuousFeedbackController.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
using namespace mixsense;
using namespace std::chrono_literals;
using Clock = ControllerBridge::Clock;
constexpr FeedbackAddress haptic { FeedbackTarget::Haptic, 0 };
void require (bool value, const char* description)
{
    if (! value) throw std::runtime_error (description);
}
bool near (float a, float b) { return std::abs (a - b) < 0.001f; }

class TestTransport final : public IControllerTransport
{
public:
    bool connect() override { return device.connect(); }
    void disconnect() override { device.disconnect(); }
    bool isConnected() const override { return device.isConnected(); }
    std::uint64_t sessionId() const noexcept override { return device.sessionId(); }
    std::optional<TransportMessage> receive() override { return device.receive(); }
    bool send (const TransportMessage& message) override { return ! failSend && device.send (message); }
    bool failSend = false;
    InMemoryControllerTransport device;
};

void takeoverTests()
{
    AudioPluginAudioProcessor processor;
    auto& state = processor.getParameterState();
    ControllerParameterAdapter adapter (state);
    InMemoryControllerTransport device;
    ControllerBridge bridge (device, [&] (const auto& event) { require (adapter.apply (event), "apply control"); },
                             [&] { adapter.resetTakeoverSession(); });
    auto* threshold = state.getParameter ("threshold");
    auto* ratio = state.getParameter ("ratio");
    threshold->setValueNotifyingHost (0.8f);
    ratio->setValueNotifyingHost (0.3f);
    bridge.connect();
    device.inject ({ MessageKind::Snapshot, 0, 0.1f });
    device.inject ({ MessageKind::Snapshot, 1, 0.9f });
    bridge.service();
    require (near (threshold->getValue(), 0.8f) && near (ratio->getValue(), 0.3f), "initial snapshots preserve DAW parameters");
    require (! adapter.hasPhysicalTakeover (ControlId::Threshold) && ! adapter.hasPhysicalTakeover (ControlId::Ratio), "independent controls begin untaken");
    device.inject ({ MessageKind::Value, 0, 0.25f });
    device.inject ({ MessageKind::Snapshot, 0, 0.95f }); // Delayed startup publication.
    bridge.service();
    require (near (threshold->getValue(), 0.25f) && adapter.hasPhysicalTakeover (ControlId::Threshold), "first genuine movement takes over");
    require (! adapter.hasPhysicalTakeover (ControlId::Ratio) && near (ratio->getValue(), 0.3f), "other control remains independent");
    device.inject ({ MessageKind::Value, 0, 0.4f });
    bridge.service();
    require (near (threshold->getValue(), 0.4f), "repeated movement edits normally");
    // UI attachments and DAW automation use this same parameter path.
    threshold->beginChangeGesture(); threshold->setValueNotifyingHost (0.7f); threshold->endChangeGesture();
    require (near (threshold->getValue(), 0.7f), "UI/automation remains authoritative until movement");
    device.inject ({ MessageKind::Snapshot, 0, 0.4f });
    bridge.service();
    require (near (threshold->getValue(), 0.7f), "snapshot cannot undo UI/automation edit after takeover");
    device.inject ({ MessageKind::Value, 0, 0.5f });
    device.inject ({ MessageKind::Value, 1, 0.6f });
    bridge.service();
    require (near (threshold->getValue(), 0.5f) && near (ratio->getValue(), 0.6f)
             && adapter.hasPhysicalTakeover (ControlId::Ratio), "each movement independently takes over");
    juce::MemoryBlock saved;
    processor.getStateInformation (saved);
    threshold->setValueNotifyingHost (0.2f);
    processor.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    device.inject ({ MessageKind::Snapshot, 0, 0.2f });
    bridge.service();
    require (near (threshold->getValue(), 0.5f), "snapshot cannot undo preset restoration");
    // The bridge must notice a whole session transition between service calls.
    device.disconnect(); device.connect();
    device.inject ({ MessageKind::Snapshot, 0, 0.1f });
    bridge.service();
    require (! adapter.hasPhysicalTakeover (ControlId::Threshold) && ! adapter.hasPhysicalTakeover (ControlId::Ratio), "session resets per-control takeover");
    require (near (threshold->getValue(), 0.5f), "reconnect snapshot preserves saved state");
    device.inject ({ MessageKind::Value, 0, 0.1f });
    bridge.service();
    require (near (threshold->getValue(), 0.1f), "movement after reconnect accepted without pickup");
}

void persistentFeedbackTests()
{
    InMemoryControllerTransport device;
    ControllerBridge bridge (device, {});
    bridge.connect();
    ContinuousFeedbackController feedback (bridge);
    require (! feedback.configure ({ 0ms, 350ms }), "refresh settings require positive durations");
    require (feedback.configure ({ 100ms, 350ms }), "test-only refresh and source age settings");
    const auto t = Clock::time_point {};
    const auto tick = [&] (auto now) { feedback.service (now); bridge.service (now); };
    require (feedback.publish (0.5f, true, 1, t), "fresh source starts continuous feedback");
    tick (t);
    require (device.sentMessages().size() == 1 && near (device.sentMessages().back().value, 0.5f), "initial level sent promptly");
    tick (t + 99ms);
    require (device.sentMessages().size() == 1, "no early refresh");
    tick (t + 100ms);
    require (device.sentMessages().size() == 2 && near (device.sentMessages().back().value, 0.5f), "unchanged intensity refreshed");
    require (feedback.publish (0.5f, true, 2, t + 200ms), "new metric generation renews source freshness");
    tick (t + 200ms);
    require (! feedback.publish (0.9f, true, 2, t + 300ms), "re-observing cached generation cannot change or renew state");
    tick (t + 500ms);
    require (feedback.active() && near (device.sentMessages().back().value, 0.5f), "fresh source can maintain a steady level");
    bridge.setFeedbackInterval (10s); // A stop must bypass this ordinary level limit.
    tick (t + 550ms);
    require (! feedback.active() && device.sentMessages().back().value == 0.0f, "expired source stops with immediate zero despite live transport");
    const auto stoppedCount = device.sentMessages().size();
    tick (t + 1s);
    require (device.sentMessages().size() == stoppedCount, "stale data never refreshed indefinitely");
    require (! feedback.publish (0.5f, true, 2, t + 1s), "expired generation cannot reactivate feedback");
    bridge.setFeedbackInterval (0ms);
    feedback.publish (0.75f, true, 3, t + 1100ms); tick (t + 1100ms);
    require (near (device.sentMessages().back().value, 0.75f), "new intensity replaces old target");
    feedback.clear (t + 1101ms); tick (t + 1101ms);
    require (! feedback.active() && device.sentMessages().back().value == 0.0f, "explicit stop requests zero");
    feedback.publish (0.6f, true, 4, t + 1200ms); tick (t + 1200ms);
    require (! feedback.publish (std::numeric_limits<float>::quiet_NaN(), true, 5, t + 1201ms), "invalid source rejected");
    tick (t + 1201ms);
    require (device.sentMessages().back().value == 0.0f, "invalid source requests zero");
    require (! feedback.publish (0.6f, true, 5, t + 1202ms), "invalid generation cannot later be reused as fresh");

    feedback.publish (0.6f, true, 6, t + 1300ms); tick (t + 1300ms);
    const auto beforeSession = device.sentMessages().size();
    device.disconnect(); device.connect(); // No intervening application service.
    tick (t + 1400ms);
    require (! feedback.active() && device.sentMessages().size() == beforeSession, "new session never refreshes previous nonzero state");
    require (! feedback.publish (0.6f, true, 6, t + 1401ms), "cached metric cannot revive prior-session intensity");
    feedback.publish (0.2f, true, 7, t + 1402ms); tick (t + 1402ms);
    require (near (device.sentMessages().back().value, 0.2f), "new source publication enables new session");
    bridge.disconnect();
    require (! feedback.publish (1.0f, true, 8, t + 1500ms), "disconnected source is not queued");
    tick (t + 1600ms);
    bridge.connect();
    require (! feedback.publish (1.0f, true, 8, t + 1700ms), "publication observed while disconnected is not replayed");
    feedback.publish (0.3f, true, 9, t + 1800ms); tick (t + 1800ms);
    require (! feedback.publish (0.3f, false, 10, t + 1801ms), "explicit metric invalidity is rejected");
    tick (t + 1801ms);
    require (! feedback.active() && device.sentMessages().back().value == 0.0f, "metric invalidity deactivates and requests zero");
}

void applicationFeedbackApiTests()
{
    auto device = std::make_unique<InMemoryControllerTransport>();
    AudioPluginAudioProcessor processor (std::move (device));
    require (processor.connectController ("MOCK"), "application API connects mock");
    require (processor.configureContinuousFeedback ({ 100ms, 500ms }), "application configures continuous settings");
    require (processor.startDevelopmentFeedback (0.5f) && processor.developmentFeedbackActive(), "development facility starts synthetic source");
    require (processor.startDevelopmentFeedback (0.75f), "development facility updates latest level");
    processor.clearContinuousFeedback();
    require (! processor.developmentFeedbackActive(), "development facility explicitly stops");
    require (processor.publishMappedHapticFeedback (0.4f, true, 10), "mapper publishes fresh generation");
    require (! processor.publishMappedHapticFeedback (0.9f, true, 10), "application adapter does not mint freshness for cached metrics");
    require (! processor.publishMappedHapticFeedback (0.4f, false, 11), "invalid mapped metric rejected");
    require (! processor.publishMappedHapticFeedback (0.4f, true, 11), "same invalid generation cannot revive mapped state");
    require (processor.publishMappedHapticFeedback (0.4f, true, 12), "new valid mapped metric accepted");
    processor.disconnectController(); processor.connectController ("MOCK");
    require (! processor.publishMappedHapticFeedback (0.4f, true, 12), "application cannot replay old mapped generation after reconnect");
    require (processor.publishMappedHapticFeedback (0.2f, true, 13), "fresh mapped generation can start new session");
}

void boundedRetryTests()
{
    TestTransport transport;
    ControllerBridge bridge (transport, {});
    bridge.connect();
    ContinuousFeedbackController feedback (bridge);
    feedback.configure ({ 100ms, 1s });
    const auto t = Clock::time_point {};
    transport.failSend = true;
    for (std::size_t i = 1; i <= ControllerBridge::pendingCapacity; ++i)
        require (bridge.submitFeedback ({ { FeedbackTarget::Haptic, static_cast<std::uint16_t> (i) }, FeedbackLevel { 0.1f } }), "fill bounded pending states");
    // The application keeps ONE desired target, regardless of local rejection.
    for (std::uint64_t i = 1; i <= 1000; ++i)
    {
        feedback.publish (i == 1000 ? 0.75f : 0.25f, true, i, t);
        feedback.service (t);
    }
    require (transport.device.sentMessages().empty(), "failed submissions never falsely marked sent");
    require (! bridge.submitFeedback ({ haptic, FeedbackLevel { 0.5f } }), "pending state remains bounded");
    transport.failSend = false;
    bridge.service (t);
    feedback.service (t + 1ms); bridge.service (t + 1ms);
    require (transport.device.sentMessages().back().id == 0 && near (transport.device.sentMessages().back().value, 0.75f), "retry sends newest desired state");
    transport.failSend = true;
    feedback.clear (t + 2ms);
    bridge.service (t + 2ms);
    feedback.publish (0.9f, true, 1001, t + 3ms);
    feedback.service (t + 2s); // Expires before any pending nonzero was written.
    transport.failSend = false;
    bridge.service (t + 2s);
    require (transport.device.sentMessages().back().value == 0.0f, "stale zero supersedes pending rejected nonzero");

    // Exercise failed ZERO submission too: the endpoint has no slot in a full
    // bridge. A single stop intent survives until the bounded queue can drain.
    InMemoryControllerTransport fullDevice;
    ControllerBridge fullBridge (fullDevice, {});
    fullBridge.connect();
    ContinuousFeedbackController stopRetry (fullBridge);
    stopRetry.configure ({ 100ms, 1s });
    for (std::size_t i = 1; i <= ControllerBridge::pendingCapacity; ++i)
        fullBridge.submitFeedback ({ { FeedbackTarget::Haptic, static_cast<std::uint16_t> (i) }, FeedbackLevel { 0.1f } });
    stopRetry.clear (t);
    for (int i = 0; i < 1000; ++i) stopRetry.service (t);
    fullBridge.service (t);
    stopRetry.service (t + 1ms); fullBridge.service (t + 1ms);
    require (fullDevice.sentMessages().size() == ControllerBridge::pendingCapacity + 1
             && fullDevice.sentMessages().back().id == 0 && fullDevice.sentMessages().back().value == 0.0f, "failed zero retries once capacity returns without accumulation");
}

void pulseContractTests()
{
    InMemoryControllerTransport device;
    ControllerBridge bridge (device, {});
    bridge.connect();
    ContinuousFeedbackController feedback (bridge);
    feedback.configure ({ 100ms, 1s });
    const auto t = Clock::time_point {};
    feedback.publish (0.4f, true, 1, t); bridge.service (t);
    bridge.submitFeedback ({ haptic, FeedbackPulse { 0.9f, 100 } });
    bridge.service (t + 1ms);
    feedback.publish (0.6f, true, 2, t + 50ms); bridge.service (t + 50ms);
    feedback.service (t + 150ms); bridge.service (t + 150ms);
    const auto& sent = device.sentMessages();
    require (sent.size() == 4 && sent[1].kind == MessageKind::Pulse && sent[1].durationMs == 100, "pulse delivered once without host sleep");
    require (near (sent[0].value, 0.4f) && near (sent[2].value, 0.6f) && near (sent[3].value, 0.6f), "continuous state survives pulse and refreshes newest baseline");
    // These are host message assertions only. Exact pulse timing and resuming
    // the latest baseline must be checked against the firmware bench test plan.
    require (! bridge.submitFeedback ({ haptic, FeedbackPulse { 1.0f, 0 } }), "invalid pulse duration rejected");
    bridge.submitFeedback ({ haptic, FeedbackPulse { 0.8f, 10 } });
    bridge.submitFeedback ({ haptic, FeedbackPulse { 0.7f, 20 } });
    bridge.service (t + 160ms);
    require (sent[sent.size() - 2].durationMs == 10 && sent.back().durationMs == 20, "pulses retain individual FIFO order");
    bridge.submitFeedback ({ haptic, FeedbackPulse { 0.8f, 30 } });
    const auto before = sent.size();
    device.disconnect(); device.connect();
    bridge.service (t + 170ms);
    require (sent.size() == before, "session change cancels pending pulses even between ticks");
}

void backgroundDestructionTest()
{
    auto processor = std::make_unique<AudioPluginAudioProcessor>();
    std::atomic<bool> finished { false };
    std::thread destroyer ([processor = std::move (processor), &finished] () mutable
    {
        processor.reset();
        finished = true;
    });
    // Deliberately do not dispatch messages while awaiting the destructor. The
    // old MessageManagerLock destructor would deadlock this host-like sequence.
    const auto deadline = Clock::now() + 2s;
    while (! finished && Clock::now() < deadline) std::this_thread::sleep_for (1ms);
    if (! finished)
    {
        // Avoid std::terminate on an assertion; CTest's process timeout reports
        // an actual deadlock if joining fails. No detached thread can outlive refs.
        destroyer.join();
        throw std::runtime_error ("background processor destruction waited for message dispatch");
    }
    destroyer.join();
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    try
    {
        takeoverTests();
        persistentFeedbackTests();
        applicationFeedbackApiTests();
        boundedRetryTests();
        pulseContractTests();
        backgroundDestructionTest();
        std::cout << "Snapshot/movement takeover, fresh persistent feedback, pulse separation and background teardown passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
