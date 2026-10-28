#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "controller/SerialPortName.h"
#include "GrHapticMapping.h"
#include <mutex>
#include <cmath>

// Keep the timer alive until deletion can run on its dispatch thread. Detaching
// its callback waits only for in-flight memory/parameter work, never for message
// dispatch. This avoids MessageManagerLock deadlock when a host's UI thread waits
// for a background plugin-destructor thread.
struct AudioPluginAudioProcessor::ControllerServiceTimer final : juce::Timer
{
    explicit ControllerServiceTimer (std::function<void()> service) : callback (std::move (service)) { startTimer (10); }
    ~ControllerServiceTimer() override { stopTimer(); }
    void detach()
    {
        stopTimer();
        const std::lock_guard<std::mutex> lock (mutex);
        callback = {};
    }
    void timerCallback() override
    {
        const std::lock_guard<std::mutex> lock (mutex);
        if (callback) callback();
    }
    std::mutex mutex;
    std::function<void()> callback;
};

//==============================================================================
AudioPluginAudioProcessor::AudioPluginAudioProcessor (std::unique_ptr<mixsense::IControllerTransport> transport)
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       ),
       parameters (*this, nullptr, "MixSenseParameters", mixsense::ControllerParameterAdapter::createLayout()),
       controllerParameters (parameters),
       controllerTransport (transport ? std::move (transport) : std::make_unique<mixsense::SerialControllerTransport>()),
       controllerBridge (*controllerTransport, [this] (const auto& event) { applyControllerEvent (event); }, [this]
       {
           controllerParameters.resetTakeoverSession();
       }),
       continuousFeedback (controllerBridge)
{
    thresholdParameter = parameters.getRawParameterValue("threshold");
    ratioParameter = parameters.getRawParameterValue("ratio");
    attackParameter = parameters.getRawParameterValue("attack");
    releaseParameter = parameters.getRawParameterValue("release");
    makeupParameter = parameters.getRawParameterValue("makeupGain");
    bypassParameter = parameters.getRawParameterValue("bypass");
    // Message-thread scheduling only. Device I/O lives on the serial worker.
    serviceTimer = std::make_shared<ControllerServiceTimer> ([this] { serviceController(); });
}

AudioPluginAudioProcessor::~AudioPluginAudioProcessor()
{
    serviceTimer->detach();
    {
        auto timer = std::move (serviceTimer);
        if (const auto* manager = juce::MessageManager::getInstanceWithoutCreating(); manager && ! manager->isThisTheMessageThread())
            juce::MessageManager::callAsync ([timer] { (void) timer; });
    }
    controllerBridge.disconnect();
}

void AudioPluginAudioProcessor::serviceController()
{
    const auto session = controllerBridge.sessionGeneration();
    if (grFeedbackEnabled && session != grFeedbackSession) grFeedbackEnabled = false;
    if (session != developmentFeedbackSession) developmentFeedbackEnabled = false;
    developmentFeedbackSession = session;
    const auto now = mixsense::ControllerBridge::Clock::now();
    if (grFeedbackEnabled)
    {
        if (const auto metric = audioMeters.read(); metric && metric->generation > lastGrPublication)
        {
            lastGrPublication = metric->generation;
            continuousFeedback.publish(mixsense::grhaptic::normalizedDrive(metric->reductionDb, metric->inputPeakDb),
                metric->audioRunning, ++feedbackPublicationSequence, now);
        }
    }
    if (developmentFeedbackEnabled)
        continuousFeedback.publish (developmentFeedbackLevel, true, ++feedbackPublicationSequence, now);
    continuousFeedback.service (now);
    controllerBridge.service (now);
}

bool AudioPluginAudioProcessor::configureContinuousFeedback (mixsense::ContinuousFeedbackController::Settings settings)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    return continuousFeedback.configure (settings);
}

bool AudioPluginAudioProcessor::publishMappedHapticFeedback (float normalized, bool valid, std::uint64_t sourceGeneration)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    developmentFeedbackEnabled = false;
    grFeedbackEnabled = false;
    if (valid && lastMappedGeneration && sourceGeneration <= *lastMappedGeneration) return false;
    if (! lastMappedGeneration || sourceGeneration > *lastMappedGeneration) lastMappedGeneration = sourceGeneration;
    return continuousFeedback.publish (normalized, valid, ++feedbackPublicationSequence, mixsense::ControllerBridge::Clock::now());
}

void AudioPluginAudioProcessor::clearContinuousFeedback()
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    developmentFeedbackEnabled = false;
    grFeedbackEnabled = false;
    continuousFeedback.clear (mixsense::ControllerBridge::Clock::now());
}

bool AudioPluginAudioProcessor::startDevelopmentFeedback (float normalized)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    grFeedbackEnabled = false;
    developmentFeedbackSession = controllerBridge.sessionGeneration();
    const bool accepted = continuousFeedback.publish (normalized, true, ++feedbackPublicationSequence, mixsense::ControllerBridge::Clock::now());
    developmentFeedbackEnabled = accepted && continuousFeedback.active();
    developmentFeedbackLevel = normalized;
    return accepted;
}

bool AudioPluginAudioProcessor::setGrHapticsEnabled(bool enabled)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    clearContinuousFeedback();
    if (!enabled) return true;
    if (!controllerBridge.isConnected()) return false;
    // Require an audio publication AFTER explicit activation; cached data is not fresh.
    const auto metric = audioMeters.read();
    if (!metric) return false;
    lastGrPublication = metric->generation;
    grFeedbackSession = controllerBridge.sessionGeneration();
    continuousFeedback.configure({std::chrono::milliseconds(50), std::chrono::milliseconds(250)});
    grFeedbackEnabled = true;
    return true;
}

bool AudioPluginAudioProcessor::connectController (const juce::String& port)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto name = mixsense::normalizeSerialPortName (port.trim().toStdString());
    if (auto* serial = dynamic_cast<mixsense::SerialControllerTransport*> (controllerTransport.get()))
        if (! serial->setPort (name)) return false;
    controllerPort = juce::String::fromUTF8 (name.c_str());
    return controllerBridge.connect();
}

void AudioPluginAudioProcessor::disconnectController()
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    controllerBridge.disconnect();
}

mixsense::ControllerConnectionState AudioPluginAudioProcessor::controllerConnectionState() const
{
    if (auto* serial = dynamic_cast<mixsense::SerialControllerTransport*> (controllerTransport.get()))
        return serial->connectionState();
    return controllerTransport->isConnected() ? mixsense::ControllerConnectionState::Connected
                                               : mixsense::ControllerConnectionState::Disconnected;
}

mixsense::ControllerConnectionError AudioPluginAudioProcessor::controllerConnectionError() const
{
    if (auto* serial = dynamic_cast<mixsense::SerialControllerTransport*> (controllerTransport.get()))
        return serial->connectionError();
    return mixsense::ControllerConnectionError::None;
}

bool AudioPluginAudioProcessor::submitControllerFeedback (mixsense::FeedbackCommand command)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    return controllerBridge.submitFeedback (std::move (command));
}

void AudioPluginAudioProcessor::setControllerFeedbackInterval (mixsense::ControllerBridge::Clock::duration interval)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    controllerBridge.setFeedbackInterval (interval);
}

//==============================================================================
const juce::String AudioPluginAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool AudioPluginAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool AudioPluginAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool AudioPluginAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double AudioPluginAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int AudioPluginAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int AudioPluginAudioProcessor::getCurrentProgram()
{
    return 0;
}

void AudioPluginAudioProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String AudioPluginAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void AudioPluginAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

//==============================================================================
void AudioPluginAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(samplesPerBlock);
    audioSampleRate = std::isfinite(sampleRate) && sampleRate > 0.0 ? sampleRate : 48000.0;
    testTonePhase = 0.0;
    compressor.prepare(audioSampleRate);
    audioMeters.publish(0.0f, -100.0f, false);
}

void AudioPluginAudioProcessor::releaseResources()
{
    compressor.reset();
    audioMeters.publish(0.0f, -100.0f, false);
}

bool AudioPluginAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    // Some plugin hosts, such as certain GarageBand versions, will only
    // load plugins that support stereo bus layouts.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}

void AudioPluginAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                              juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused(midiMessages);
    processAudio(buffer, false);
}

void AudioPluginAudioProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer,
                                                    juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused(midiMessages);
    processAudio(buffer, true);
}

void AudioPluginAudioProcessor::processAudio(juce::AudioBuffer<float>& buffer, bool forceBypass)
{
    juce::ScopedNoDenormals noDenormals;
    const int channels = std::min(getTotalNumInputChannels(), buffer.getNumChannels());
    const int samples = buffer.getNumSamples();
    for (int channel = channels; channel < buffer.getNumChannels(); ++channel)
        buffer.clear(channel, 0, samples);
    if (channels == 0 || samples == 0)
    {
        audioMeters.publish(0.0f, -100.0f, false);
        return;
    }

    auto* const* data = buffer.getArrayOfWritePointers();
    if (testToneEnabled.load(std::memory_order_relaxed))
    {
        constexpr double twoPi = 6.2831853071795864769;
        constexpr float amplitude = 0.12589254f; // -18 dBFS peak, optional test source.
        const double increment = twoPi * 1000.0 / audioSampleRate;
        for (int sample = 0; sample < samples; ++sample)
        {
            const float value = amplitude * static_cast<float>(std::sin(testTonePhase));
            testTonePhase += increment;
            if (testTonePhase >= twoPi) testTonePhase -= twoPi;
            for (int channel = 0; channel < channels; ++channel) data[channel][sample] = value;
        }
    }
    const mixsense::DemoCompressor::Parameters settings {
        thresholdParameter->load(std::memory_order_relaxed), ratioParameter->load(std::memory_order_relaxed),
        attackParameter->load(std::memory_order_relaxed), releaseParameter->load(std::memory_order_relaxed),
        makeupParameter->load(std::memory_order_relaxed)
    };
    const bool bypassed = forceBypass || bypassParameter->load(std::memory_order_relaxed) >= 0.5f;
    const auto result = compressor.process(data, channels, samples, settings, bypassed);
    const float inputDb = 20.0f * std::log10(std::max(result.inputPeak, 1.0e-5f));
    // Lock-free publication only. No bridge, serial, UI calls or feedback mapping here.
    audioMeters.publish(result.maximumReductionDb, inputDb, true);
}

//==============================================================================
bool AudioPluginAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* AudioPluginAudioProcessor::createEditor()
{
    return new AudioPluginAudioProcessorEditor (*this);
}

//==============================================================================
void AudioPluginAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (const auto xml = parameters.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void AudioPluginAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (const auto xml = getXmlFromBinary (data, sizeInBytes); xml && xml->hasTagName (parameters.state.getType()))
        parameters.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorParameter* AudioPluginAudioProcessor::getBypassParameter() const
{
    return parameters.getParameter (mixsense::ControllerParameterAdapter::parameterId (mixsense::ControlId::Bypass));
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AudioPluginAudioProcessor();
}
