#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

/*  The plug-in window: the Sudz editor (ui/index.html, compiled into the plug-in) in a web view.

    Page -> plug-in (native functions):
      pwGetState()                    sample rate, whether a transfer exists, saved edits
      pwArm(bool)                     arm / finish a transfer
      pwSaveProject(json)             store the edits in the GarageBand project
      pwPutRendered(offset, base64)   tuned samples (32-bit float, little-endian) for playback
      pwPlayOriginal(bool)            A/B: play the track untouched
    Plug-in -> page (events):
      pwTransferState {armed, recording, seconds, timeline}
      pwTransferDone  {length, sampleRate}
      pwPlayhead      {t, playing}
    The page fetches the transferred recording from transfer.wav.
*/
class SudzEditor final : public juce::AudioProcessorEditor,
                                private juce::Timer
{
public:
    explicit SudzEditor (SudzProcessor&);
    ~SudzEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);
    juce::var stateObject() const;
    juce::WebBrowserComponent::Options makeOptions();

    SudzProcessor& proc;
    juce::WebBrowserComponent web;
    std::unique_ptr<juce::FileChooser> chooser;   // desktop app: native Open dialog
    juce::MemoryBlock openedBytes;                 // ...and the file it opened, served at /open/current
    double lastSentPlayhead = -1.0;
    bool lastPlaying = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SudzEditor)
};
