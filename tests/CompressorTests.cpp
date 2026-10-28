#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "GrHapticMapping.h"
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool near(float a, float b, float tolerance = 0.02f) { return std::abs(a - b) <= tolerance; }
constexpr int samples = 64;

void compressorTests()
{
    mixsense::DemoCompressor compressor;
    compressor.prepare(48000.0);
    mixsense::DemoCompressor::Parameters settings;
    constexpr float input = 0.25118864f; // -12 dBFS DC: analytic hard-knee test.
    std::array<float, samples> left, right;
    float* data[] {left.data(), right.data()};
    mixsense::DemoCompressor::Result result;
    const auto run = [&](float value, int blocks, bool bypass = false)
    {
        for (int block = 0; block < blocks; ++block)
        {
            left.fill(value); right.fill(value * 0.1f);
            result = compressor.process(data, 2, samples, settings, bypass);
        }
    };
    run(input, 1000);
    require(near(result.maximumReductionDb, 9.0f), "-12dB input, -24dB threshold, 4:1 gives 9dB GR");
    require(near(20.0f * std::log10(input / left.back()), 9.0f), "GR equals actual audio attenuation");
    require(near(right.back() / left.back(), 0.1f, 0.00001f), "stereo channels share one gain");
    settings.makeupDb = 6.0f;
    run(input, 2);
    require(near(result.maximumReductionDb, 9.0f), "makeup does not change GR measurement");
    require(near(20.0f * std::log10(input / left.back()), 3.0f), "makeup is applied after compression");
    run(input, 1, true);
    require(left.back() == input && result.maximumReductionDb == 0, "bypass is exact dry audio and zero GR");
    settings.makeupDb = 0; settings.ratio = 1;
    run(input, 1);
    require(left.back() == input && result.maximumReductionDb == 0, "unity ratio is transparent");
    settings.ratio = 4; compressor.reset();
    run(input, 1);
    require(result.maximumReductionDb > 0 && result.maximumReductionDb < 9, "attack develops over time");
    run(input, 1000);
    run(0, 1);
    require(result.maximumReductionDb > 0 && left.back() == 0, "release decays while silent samples stay silent");
    run(0, 2000);
    require(result.maximumReductionDb < 0.001f, "silence releases the meter to zero");
    compressor.prepare(96000);
    run(0.01f, 10);
    require(result.maximumReductionDb == 0 && left.back() == 0.01f, "below-threshold audio is transparent after reset");
    require(compressor.process(data, 0, samples, settings, false).maximumReductionDb == 0, "empty layout handled");
    settings.thresholdDb = -36;
    run(0.12589254f, 2000);
    require(near(result.maximumReductionDb, 13.5f), "constant -18 dB at -36 dB / 4:1 gives 13.5 dB GR");
}

void coherentMeterTests()
{
    mixsense::AudioMeterState meters;
    std::atomic<bool> finished {false};
    std::thread writer([&]
    {
        for (int i = 0; i < 20000; ++i)
        {
            const float reduction = static_cast<float>(i % 24);
            meters.publish(reduction, -reduction - 10.0f, true);
        }
        finished = true;
    });
    bool coherent = true;
    while (!finished.load())
        if (const auto snapshot = meters.read(); snapshot && snapshot->audioRunning)
            coherent = coherent && snapshot->inputPeakDb == -snapshot->reductionDb - 10.0f
                       && (snapshot->generation & 1u) == 0;
    writer.join();
    require(coherent, "concurrent meter snapshots stay coherent");
    require(meters.read()->generation == 40000, "only producer publications advance generation");
}

void setParameter(AudioPluginAudioProcessor& processor, const char* name, float value)
{
    auto* parameter = processor.getParameterState().getParameter(name);
    require(parameter != nullptr, "parameter exists");
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

void processorTests(const juce::String& renderPath)
{
    AudioPluginAudioProcessor processor;
    processor.prepareToPlay(48000, samples);
    juce::AudioBuffer<float> audio(2, samples);
    juce::MidiBuffer midi;
    audio.clear(); processor.processBlock(audio, midi);
    auto meters = processor.getAudioMeters();
    require(meters && meters->audioRunning && meters->reductionDb == 0, "silence has zero GR");
    require(audio.getMagnitude(0, samples) == 0, "test tone is off by default");
    for (int i = 0; i < 1000; ++i)
    {
        for (int c = 0; c < 2; ++c) audio.clear(c, 0, samples);
        for (int c = 0; c < 2; ++c)
            for (int s = 0; s < samples; ++s) audio.setSample(c, s, 0.25118864f);
        processor.processBlock(audio, midi);
    }
    meters = processor.getAudioMeters();
    require(meters && near(meters->reductionDb, 9), "APVTS defaults drive real compressor");
    setParameter(processor, "bypass", 1);
    audio.clear(); audio.setSample(0, 0, 0.4f); processor.processBlock(audio, midi);
    require(audio.getSample(0, 0) == 0.4f && processor.getAudioMeters()->reductionDb == 0, "APVTS bypass zeros GR");
    setParameter(processor, "bypass", 0);
    processor.setTestToneEnabled(true);
    setParameter(processor, "threshold", -36);
    for (int i = 0; i < 1000; ++i) { audio.clear(); processor.processBlock(audio, midi); }
    meters = processor.getAudioMeters();
    require(meters && near(meters->reductionDb, 12.3f, 0.3f) && near(meters->inputPeakDb, -18, 0.2f),
            "sample detector plus attack/release gives about 12.3 dB for -18 dBFS peak sine");

    if (renderPath.isNotEmpty())
    {
        auto editor = std::unique_ptr<juce::AudioProcessorEditor>(processor.createEditor());
        editor->setVisible(true);
        juce::Timer::callPendingTimersSynchronously();
        const auto image = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.5f);
        juce::FileOutputStream stream {juce::File(renderPath)};
        require(stream.openedOk(), "open UI render output");
        require(stream.setPosition(0) && stream.truncate().wasOk(), "replace previous UI render");
        juce::PNGImageFormat format;
        require(format.writeImageToStream(image, stream), "render GR editor");
        editor.reset();
        require(!processor.isTestToneEnabled(), "closing editor stops development tone");
    }
    processor.setTestToneEnabled(false);
    audio.clear(); audio.setSample(0, 0, 0.5f); processor.processBlockBypassed(audio, midi);
    require(audio.getSample(0, 0) == 0.5f && processor.getAudioMeters()->reductionDb == 0, "host bypass clears GR");
    processor.releaseResources();
    require(!processor.getAudioMeters()->audioRunning, "audio stop invalidates meter source");
}

template<class Predicate> void waitFor(Predicate ready, const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!ready())
    {
        require(std::chrono::steady_clock::now() < deadline, message);
        juce::Timer::callPendingTimersSynchronously();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void grFeedbackTests()
{
    using namespace mixsense;
    require(grhaptic::normalizedDrive(0.5f, -18) == 0, "GR deadband is motor off");
    require(grhaptic::normalizedDrive(12, -100) == 0, "silence is motor off despite release tail");
    require(grhaptic::boundedRtp(1) == 90 && grhaptic::boundedRtp(-1) == 0, "firmware RTP cap");
    require(grhaptic::normalizedDrive(6, -18) < grhaptic::normalizedDrive(12, -18), "more GR gives more drive");

    auto device = std::make_unique<InMemoryControllerTransport>();
    auto* observed = device.get();
    AudioPluginAudioProcessor processor(std::move(device));
    require(!processor.setGrHapticsEnabled(true), "cannot arm GR feedback while disconnected");
    processor.connectController("MOCK");
    // The same logical FSR movement decoded by USB edits the actual DSP parameter.
    observed->inject({MessageKind::Value, 0, 0.4f});
    waitFor([&] { return near(processor.getParameterState().getRawParameterValue("threshold")->load(), -36); },
            "FSR movement reaches threshold");
    processor.prepareToPlay(48000, samples);
    processor.setTestToneEnabled(true);
    juce::AudioBuffer<float> audio(2, samples);
    juce::MidiBuffer midi;
    for (int i = 0; i < 1000; ++i) { audio.clear(); processor.processBlock(audio, midi); }
    require(observed->sentMessages().empty(), "audio never enables haptics implicitly");
    require(processor.setGrHapticsEnabled(true), "explicitly arm connected GR source");
    const auto waitForLevel = [&](bool nonzero)
    {
        waitFor([&] { return !observed->sentMessages().empty()
            && (observed->sentMessages().back().value > 0) == nonzero; }, "expected mapped level or stop");
    };
    // The cached pre-enable measurement is deliberately not submitted.
    require(observed->sentMessages().empty(), "arming does not synchronously send cached GR");
    audio.clear(); processor.processBlock(audio, midi);
    waitForLevel(true);
    require(near(observed->sentMessages().back().value,
        grhaptic::normalizedDrive(processor.getAudioMeters()->reductionDb, -18), 0.001f), "actual measured GR maps to wire level");
    require(observed->sentMessages().back().value <= 90.0f / 127.0f, "host obeys motor cap");
    waitForLevel(false); // Polling the same generation must expire within 250 ms.
    audio.clear(); processor.processBlock(audio, midi); waitForLevel(true);
    setParameter(processor, "bypass", 1);
    audio.clear(); processor.processBlock(audio, midi); waitForLevel(false);
    setParameter(processor, "bypass", 0);
    for (int i = 0; i < 1000; ++i) { audio.clear(); processor.processBlock(audio, midi); }
    waitForLevel(true);
    processor.releaseResources(); waitForLevel(false);
    processor.disconnectController(); processor.connectController("MOCK");
    waitFor([&] { return !processor.grHapticsEnabled(); }, "session change disarms GR source");
    processor.prepareToPlay(48000, samples);
    audio.clear(); processor.processBlock(audio, midi);
    require(!processor.grHapticsEnabled(), "reconnect requires explicit activation");
    processor.setGrHapticsEnabled(true);
    auto editor = std::unique_ptr<juce::AudioProcessorEditor>(processor.createEditor());
    editor.reset();
    require(!processor.grHapticsEnabled(), "closing editor stops GR haptics");
}
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;
    try
    {
        compressorTests(); coherentMeterTests(); grFeedbackTests();
        processorTests(argc == 3 && juce::String(argv[1]) == "--render" ? juce::String(argv[2]) : juce::String());
        std::cout << "Compressor, actual GR, coherent meters, bypass and test tone passed\n";
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
