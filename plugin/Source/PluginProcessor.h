#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "ControllerParameterAdapter.h"
#include "controller/ControllerBridge.h"
#include "controller/SerialControllerTransport.h"
#include "ContinuousFeedbackController.h"
#include "dsp/DemoCompressor.h"
#include "dsp/AudioMeterState.h"

//==============================================================================
class AudioPluginAudioProcessor final : public juce::AudioProcessor
{
public:
    //==============================================================================
    explicit AudioPluginAudioProcessor (std::unique_ptr<mixsense::IControllerTransport> transport = nullptr);
    ~AudioPluginAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getParameterState() noexcept { return parameters; }
    bool applyControllerEvent (const mixsense::ControllerEvent& event) { return controllerParameters.apply (event); }
    juce::AudioProcessorParameter* getBypassParameter() const override;

    // Message-thread application API. Never call these from processBlock().
    bool connectController (const juce::String& port);
    void disconnectController();
    mixsense::ControllerConnectionState controllerConnectionState() const;
    mixsense::ControllerConnectionError controllerConnectionError() const;
    bool submitControllerFeedback (mixsense::FeedbackCommand command);
    void setControllerFeedbackInterval (mixsense::ControllerBridge::Clock::duration);
    const juce::String& getControllerPort() const noexcept { return controllerPort; }
    bool configureContinuousFeedback (mixsense::ContinuousFeedbackController::Settings);
    bool publishMappedHapticFeedback (float normalized, bool valid, std::uint64_t sourceGeneration);
    void clearContinuousFeedback();
    // Explicit synthetic source for the development panel; no DSP freshness claim.
    bool startDevelopmentFeedback (float normalized);
    bool developmentFeedbackActive() const noexcept { return developmentFeedbackEnabled; }
    bool setGrHapticsEnabled(bool enabled);
    bool grHapticsEnabled() const noexcept { return grFeedbackEnabled; }
    std::optional<mixsense::AudioMeterState::Snapshot> getAudioMeters() const noexcept { return audioMeters.read(); }
    void setTestToneEnabled(bool enabled) noexcept { testToneEnabled.store(enabled); }
    bool isTestToneEnabled() const noexcept { return testToneEnabled.load(); }

private:
    juce::AudioProcessorValueTreeState parameters;
    mixsense::ControllerParameterAdapter controllerParameters;
    mixsense::DemoCompressor compressor;
    mixsense::AudioMeterState audioMeters;
    std::atomic<bool> testToneEnabled { false };
    double audioSampleRate = 48000.0, testTonePhase = 0.0;
    std::atomic<float>* thresholdParameter = nullptr;
    std::atomic<float>* ratioParameter = nullptr;
    std::atomic<float>* attackParameter = nullptr;
    std::atomic<float>* releaseParameter = nullptr;
    std::atomic<float>* makeupParameter = nullptr;
    std::atomic<float>* bypassParameter = nullptr;
    void processAudio(juce::AudioBuffer<float>&, bool forceBypass);
    std::unique_ptr<mixsense::IControllerTransport> controllerTransport;
    mixsense::ControllerBridge controllerBridge;
    mixsense::ContinuousFeedbackController continuousFeedback;
    juce::String controllerPort;
    bool developmentFeedbackEnabled = false;
    bool grFeedbackEnabled = false;
    std::uint64_t grFeedbackSession = 0, lastGrPublication = 0;
    float developmentFeedbackLevel = 0.0f;
    std::uint64_t feedbackPublicationSequence = 0;
    std::uint64_t developmentFeedbackSession = 0;
    std::optional<std::uint64_t> lastMappedGeneration;
    void serviceController();
    struct ControllerServiceTimer;
    std::shared_ptr<ControllerServiceTimer> serviceTimer;
    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioPluginAudioProcessor)
};
