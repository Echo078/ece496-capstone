#pragma once

#include "PluginProcessor.h"

//==============================================================================
class AudioPluginAudioProcessorEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AudioPluginAudioProcessorEditor (AudioPluginAudioProcessor&);
    ~AudioPluginAudioProcessorEditor() override;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // This reference is provided as a quick way for your editor to
    // access the processor object that created it.
    AudioPluginAudioProcessor& processorRef;
    juce::Slider threshold;
    juce::ToggleButton bypass { "Bypass" };
    juce::Label thresholdLabel { "thresholdLabel", "Threshold (dB)" };
    juce::AudioProcessorValueTreeState::SliderAttachment thresholdAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment bypassAttachment;
    juce::TextEditor portName;
    juce::TextButton connectButton { "Connect CDC" }, disconnectButton { "Disconnect" };
    juce::Label connectionLabel;
    juce::Label feedbackLabel { "feedbackLabel", "CDC test: normalized haptic intensity" };
    juce::Slider feedbackIntensity;
    juce::TextButton sendFeedbackButton { "Start / update persistent" }, stopFeedbackButton { "Stop / request zero" };
    juce::Label timingLabel { "timing", "Development settings: refresh ms / source max age ms" };
    juce::TextEditor feedbackRefreshMs, feedbackSourceMaxAgeMs;
    juce::Label feedbackResult;
    juce::Label reductionLabel { "gainReduction", "Gain reduction" };
    juce::Label reductionValue { "gainReductionValue", "0.0 dB" };
    juce::Label inputLevelLabel { "inputLevel", "Audio inactive" };
    juce::Label compressorSettings;
    juce::ToggleButton testTone { "Test tone (-18 dBFS)" };
    juce::ToggleButton grHaptics { "Enable GR haptics (ERM)" };
    juce::Label grHapticsHint { "grHapticsHint", "More gain reduction = stronger vibration; silence = off" };
    juce::Rectangle<int> reductionBar;
    float displayedReduction = 0.0f;
    std::uint64_t lastMeterGeneration = 0;
    double lastMeterPublicationMs = 0.0;
    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioPluginAudioProcessorEditor)
};
