#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
AudioPluginAudioProcessorEditor::AudioPluginAudioProcessorEditor (AudioPluginAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p),
      thresholdAttachment (p.getParameterState(), mixsense::ControllerParameterAdapter::parameterId (mixsense::ControlId::Threshold), threshold),
      bypassAttachment (p.getParameterState(), mixsense::ControllerParameterAdapter::parameterId (mixsense::ControlId::Bypass), bypass)
{
    threshold.setSliderStyle (juce::Slider::LinearHorizontal);
    threshold.setTextBoxStyle (juce::Slider::TextBoxRight, false, 80, 24);
    addAndMakeVisible (thresholdLabel);
    addAndMakeVisible (threshold);
    addAndMakeVisible (bypass);
    addAndMakeVisible(reductionLabel);
    reductionValue.setJustificationType(juce::Justification::centredRight);
    reductionValue.setColour(juce::Label::textColourId, juce::Colours::orange);
    addAndMakeVisible(reductionValue);
    addAndMakeVisible(inputLevelLabel);
    addAndMakeVisible(compressorSettings);
    testTone.setToggleState(p.isTestToneEnabled(), juce::dontSendNotification);
    testTone.onClick = [this] { processorRef.setTestToneEnabled(testTone.getToggleState()); };
    addAndMakeVisible(testTone);
    grHaptics.onClick = [this]
    {
        const bool accepted = processorRef.setGrHapticsEnabled(grHaptics.getToggleState());
        grHaptics.setToggleState(processorRef.grHapticsEnabled(), juce::dontSendNotification);
        feedbackResult.setText(accepted ? "GR feedback updated (no device acknowledgement)"
            : "Connect CDC before enabling GR feedback", juce::dontSendNotification);
    };
    addAndMakeVisible(grHaptics);
    addAndMakeVisible(grHapticsHint);
    portName.setTextToShowWhenEmpty ("COM7 or /dev/cu.usbmodem...", juce::Colours::grey);
    portName.setTooltip ("Enter the controller's serial port: Windows COM name or the exact macOS /dev/cu.* path");
    portName.setText (p.getControllerPort(), false);
    addAndMakeVisible (portName);
    addAndMakeVisible (connectButton);
    addAndMakeVisible (disconnectButton);
    addAndMakeVisible (connectionLabel);
    addAndMakeVisible (feedbackLabel);
    feedbackIntensity.setRange (0.0, 1.0, 0.25);
    feedbackIntensity.setSliderStyle (juce::Slider::LinearHorizontal);
    feedbackIntensity.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 24);
    addAndMakeVisible (feedbackIntensity);
    addAndMakeVisible (sendFeedbackButton);
    addAndMakeVisible (stopFeedbackButton);
    addAndMakeVisible (timingLabel);
    feedbackRefreshMs.setText ("100");
    feedbackSourceMaxAgeMs.setText ("500");
    addAndMakeVisible (feedbackRefreshMs);
    addAndMakeVisible (feedbackSourceMaxAgeMs);
    addAndMakeVisible (feedbackResult);
    connectButton.onClick = [this]
    {
        feedbackResult.setText (processorRef.connectController (portName.getText()) ? "Open requested" : "Open request rejected", juce::dontSendNotification);
        timerCallback();
    };
    disconnectButton.onClick = [this] { processorRef.disconnectController(); timerCallback(); };
    sendFeedbackButton.onClick = [this]
    {
        const bool configured = processorRef.configureContinuousFeedback (
            { std::chrono::milliseconds (feedbackRefreshMs.getText().getIntValue()),
              std::chrono::milliseconds (feedbackSourceMaxAgeMs.getText().getIntValue()) });
        const bool accepted = configured && processorRef.startDevelopmentFeedback (static_cast<float> (feedbackIntensity.getValue()));
        feedbackResult.setText (accepted ? "Synthetic source active (not device acknowledgement)" : "Not started: check connection and positive timings", juce::dontSendNotification);
    };
    feedbackIntensity.onValueChange = [this]
    {
        if (processorRef.developmentFeedbackActive())
            processorRef.startDevelopmentFeedback (static_cast<float> (feedbackIntensity.getValue()));
    };
    stopFeedbackButton.onClick = [this]
    {
        processorRef.clearContinuousFeedback();
        feedbackResult.setText ("Stopped; zero requested (no acknowledgement)", juce::dontSendNotification);
    };
    timerCallback();
    startTimerHz(30);
    // Make sure that before the constructor has finished, you've set the
    // editor's size to whatever you need it to be.
    setSize (600, 700);
}

AudioPluginAudioProcessorEditor::~AudioPluginAudioProcessorEditor()
{
    stopTimer();
    processorRef.setTestToneEnabled(false);
    if (processorRef.developmentFeedbackActive() || processorRef.grHapticsEnabled()) processorRef.clearContinuousFeedback();
}

void AudioPluginAudioProcessorEditor::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    const auto meters = processorRef.getAudioMeters();
    if (meters && meters->generation != lastMeterGeneration)
    {
        lastMeterGeneration = meters->generation;
        lastMeterPublicationMs = now;
        displayedReduction = meters->audioRunning ? meters->reductionDb : 0.0f;
        const auto inputText = !meters->audioRunning ? juce::String("Audio inactive")
            : meters->inputPeakDb < -90.0f ? juce::String("Input: silence")
            : "Input peak: " + juce::String(meters->inputPeakDb, 1) + " dBFS";
        inputLevelLabel.setText(inputText, juce::dontSendNotification);
    }
    if (lastMeterPublicationMs == 0.0 || now - lastMeterPublicationMs > 250.0)
    {
        displayedReduction = 0.0f;
        inputLevelLabel.setText("Audio inactive", juce::dontSendNotification);
    }
    reductionValue.setText(juce::String(displayedReduction, 1) + " dB", juce::dontSendNotification);
    auto& parameters = processorRef.getParameterState();
    compressorSettings.setText("Ratio " + juce::String(parameters.getRawParameterValue("ratio")->load(), 1)
        + ":1   Attack " + juce::String(parameters.getRawParameterValue("attack")->load(), 1)
        + " ms   Release " + juce::String(parameters.getRawParameterValue("release")->load(), 0) + " ms",
        juce::dontSendNotification);
    repaint(reductionBar.expanded(2, 2));
    using State = mixsense::ControllerConnectionState;
    const auto state = processorRef.controllerConnectionState();
    juce::String text;
    switch (state)
    {
        case State::Disconnected: text = "Disconnected"; break;
        case State::Connecting: text = "Opening CDC device..."; break;
        case State::Connected: text = "CDC connected"; break;
        case State::Error:
            switch (processorRef.controllerConnectionError())
            {
                case mixsense::ControllerConnectionError::OpenFailed: text = "Open failed: check port/other serial applications"; break;
                case mixsense::ControllerConnectionError::ReadFailed: text = "CDC read failed / device lost"; break;
                case mixsense::ControllerConnectionError::WriteFailed: text = "CDC write failed / device lost"; break;
                case mixsense::ControllerConnectionError::ReceiveOverflow: text = "CDC event queue overflow: session ended"; break;
                default: text = "CDC worker failed"; break;
            }
            break;
    }
    connectionLabel.setText (text, juce::dontSendNotification);
    const bool available = state == State::Disconnected || state == State::Error;
    portName.setEnabled (available);
    connectButton.setEnabled (available);
    disconnectButton.setEnabled (! available);
    const bool grActive = processorRef.grHapticsEnabled();
    grHaptics.setToggleState(grActive, juce::dontSendNotification);
    grHaptics.setEnabled(state == State::Connected);
    sendFeedbackButton.setEnabled (state == State::Connected && !grActive);
    feedbackIntensity.setEnabled(!grActive);
    feedbackRefreshMs.setEnabled(!grActive);
    feedbackSourceMaxAgeMs.setEnabled(!grActive);
    stopFeedbackButton.setEnabled (state == State::Connected);
}

//==============================================================================
void AudioPluginAudioProcessorEditor::paint (juce::Graphics& g)
{
    // (Our component is opaque, so we must completely fill the background with a solid colour)
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    g.setColour (juce::Colours::white);
    g.setFont (15.0f);
    g.drawFittedText ("MixSense", 20, 12, getWidth() - 40, 30, juce::Justification::centred, 1);
    const auto bar = reductionBar.toFloat();
    g.setColour(juce::Colour(0xff20242a));
    g.fillRoundedRectangle(bar, 5.0f);
    auto fill = bar;
    fill.setWidth(bar.getWidth() * juce::jlimit(0.0f, 1.0f, displayedReduction / 24.0f));
    if (fill.getWidth() > 0.0f)
    {
        g.setColour(juce::Colours::orange);
        g.fillRoundedRectangle(fill, 5.0f);
    }
    g.setColour(juce::Colours::white.withAlpha(0.35f));
    for (int tick = 1; tick < 4; ++tick)
    {
        const float x = bar.getX() + bar.getWidth() * static_cast<float>(tick) / 4.0f;
        g.drawVerticalLine(static_cast<int>(x), bar.getY(), bar.getBottom());
    }
    g.setColour(juce::Colours::lightgrey);
    g.setFont(11.0f);
    for (int tick = 0; tick <= 4; ++tick)
    {
        const int x = reductionBar.getX() + reductionBar.getWidth() * tick / 4;
        g.drawText(juce::String(tick * 6), x - 14, reductionBar.getBottom() + 2,
                   28, 16, juce::Justification::centred);
    }
}

void AudioPluginAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (20);
    area.removeFromTop (40);
    thresholdLabel.setBounds (area.removeFromTop (24));
    threshold.setBounds (area.removeFromTop (40));
    bypass.setBounds (area.removeFromTop (36));
    auto reductionHeading = area.removeFromTop(26);
    reductionLabel.setBounds(reductionHeading.removeFromLeft(300));
    reductionValue.setBounds(reductionHeading);
    reductionBar = area.removeFromTop(24).reduced(6, 0);
    area.removeFromTop(22);
    inputLevelLabel.setBounds(area.removeFromTop(22));
    compressorSettings.setBounds(area.removeFromTop(22));
    testTone.setBounds(area.removeFromTop(30));
    area.removeFromTop (12);
    auto connectionRow = area.removeFromTop (30);
    portName.setBounds (connectionRow.removeFromLeft (300).reduced (2));
    connectButton.setBounds (connectionRow.removeFromLeft (130).reduced (2));
    disconnectButton.setBounds (connectionRow.reduced (2));
    connectionLabel.setBounds (area.removeFromTop (30));
    grHaptics.setBounds(area.removeFromTop(30));
    grHapticsHint.setBounds(area.removeFromTop(24));
    feedbackLabel.setBounds (area.removeFromTop (30));
    feedbackIntensity.setBounds (area.removeFromTop (40));
    timingLabel.setBounds (area.removeFromTop (24));
    auto timingRow = area.removeFromTop (30);
    feedbackRefreshMs.setBounds (timingRow.removeFromLeft (120).reduced (2));
    feedbackSourceMaxAgeMs.setBounds (timingRow.removeFromLeft (120).reduced (2));
    auto feedbackButtons = area.removeFromTop (32);
    sendFeedbackButton.setBounds (feedbackButtons.removeFromLeft (280).reduced (2));
    stopFeedbackButton.setBounds (feedbackButtons.reduced (2));
    feedbackResult.setBounds (area.removeFromTop (32));
}
