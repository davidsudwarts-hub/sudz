#pragma once

#include <JuceHeader.h>
#include "SudzFx.h"

/*  Sudz as an Audio Unit effect for GarageBand (and other hosts without ARA).

    It works the way Melodyne does in hosts without ARA:
      1. Press Transfer in the plug-in, then play the part in GarageBand. The plug-in records a copy
         of the track, stamped with GarageBand's timeline position.
      2. Edit the notes in the plug-in window (the Sudz editor, running in a web view).
      3. On playback (and when you export or bounce the song) the plug-in replaces the transferred
         stretch with the tuned audio, sample-accurately against the timeline. Outside that
         stretch, the track passes through untouched.

    Everything — the transferred recording, the tuned audio and every edit — is saved inside the
    GarageBand project through the plug-in state.
*/
class SudzProcessor final : public juce::AudioProcessor,
                                   private juce::Timer
{
public:
    SudzProcessor();
    ~SudzProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                       { return true; }

    const juce::String getName() const override           { return "Sudz"; }
    bool acceptsMidi() const override                     { return false; }
    bool producesMidi() const override                    { return false; }
    bool isMidiEffect() const override                    { return false; }
    double getTailLengthSeconds() const override          { return tailSeconds.load(); }

    int getNumPrograms() override                         { return 1; }
    int getCurrentProgram() override                      { return 0; }
    void setCurrentProgram (int) override                 {}
    const juce::String getProgramName (int) override      { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Called from the editor (message thread)
    void setArmed (bool shouldArm, bool asNewSection = false);

    /** Sections: several transferred stretches of the track, each with its own tuned audio and edits.
        The one being edited is the "active" one (transfer/rendered below); the others wait in `others`. */
    struct SectionInfo { juce::int64 start = 0; int length = 0; bool active = false; };
    std::vector<SectionInfo> getSections() const;
    bool selectSection (int index);       // index into getSections()
    bool deleteSection (int index);
    bool isArmed() const                                  { return armed.load(); }
    bool isRecording() const                              { return recordingNow.load(); }
    bool hasTransfer() const;
    juce::int64 getTransferStart() const                  { return transferStart; }
    int getTransferLength() const                         { return (int) transfer.size(); }
    double getTransferSampleRate() const                  { return transferRate; }
    double getHostSampleRate() const                      { return currentRate; }
    bool hostReportsTimeline() const                      { return hostHasTimeline.load(); }
    /** GarageBand's tempo (0 until the host has reported one). */
    double getHostBpm() const { return hostBpm.load(); }

    /** Other Sudz instances in this song (GarageBand runs every track's plug-ins in one process), so a double
        or harmony can be lined up with the lead. Each: an id, the track name GarageBand reports (may be empty),
        where its audio starts on the timeline, and its length and rate. */
    struct PeerInfo { int id = 0; juce::String name; juce::int64 start = 0; juce::int64 length = 0; double rate = 44100.0; };
    static std::vector<PeerInfo> listPeers (const SudzProcessor* except);
    /** A peer's tuned audio (raw where nothing has been tuned) over all its sections, as one WAV starting at
        its earliest section; empty if that instance has gone. */
    static juce::MemoryBlock peerAudioAsWav (int id);
    int getInstanceId() const                             { return instanceId; }
    juce::String getTrackName() const                     { const juce::ScopedLock sl (nameLock); return trackName; }
    void updateTrackProperties (const TrackProperties& p) override { const juce::ScopedLock sl (nameLock); trackName = p.name; }

    /** The transferred recording as a 32-bit float WAV file. */
    juce::MemoryBlock transferAsWav() const;
    void clearTransfer();

    /** Writes tuned samples into the playback buffer (offset is in samples from the transfer start). */
    void writeRendered (int offset, const float* samples, int numSamples);
    void setPlayOriginal (bool b)                         { playOriginal.store (b); }

    /** Live effects: the editor's polish settings as JSON (see SudzFx.h for the keys). */
    void setFxJson (const juce::String& json);
    juce::String getFxJson() const                        { const juce::ScopedLock sl (projectLock); return fxJson; }

    void setProjectJson (const juce::String& json)        { const juce::ScopedLock sl (projectLock); projectJson = json; }
    juce::String getProjectJson() const                   { const juce::ScopedLock sl (projectLock); return projectJson; }

    /** Seconds since the transfer start, for the editor's playhead. */
    double getPlayheadSeconds() const                     { return playheadSeconds.load(); }
    bool isHostPlaying() const                            { return hostPlaying.load(); }

    /** Set by the editor; called on the message thread when a transfer finishes. */
    std::function<void()> onTransferFinished;
    std::function<void()> onTransferStateChanged;

private:
    void timerCallback() override;
    void drainFifo();
    void finishTransfer();

    // ---- recording: the audio thread pushes (timeline position, samples) into a lock-free FIFO;
    //      the message thread drains it into the transfer buffer.
    static constexpr int fifoSize = 1 << 19;              // ~11 s at 48 kHz
    juce::AbstractFifo fifo { fifoSize };
    std::vector<float> fifoSamples;
    std::vector<juce::int64> fifoPositions;               // timeline position of each FIFO sample

    std::atomic<bool> armed { false }, recordingNow { false }, wasPlaying { false };
    std::atomic<bool> hostHasTimeline { true }, hostPlaying { false }, playOriginal { false };
    std::atomic<bool> stopSeen { false };
    std::atomic<double> playheadSeconds { 0.0 };
    juce::int64 fallbackCounter = 0;

    std::vector<float> transfer;                          // message thread only
    juce::int64 transferStart = 0;
    double transferRate = 44100.0, currentRate = 44100.0;

    // ---- playback of the tuned audio (shared with the audio thread)
    juce::SpinLock renderLock;
    std::vector<float> rendered;                          // same length/timing as `transfer`
    std::atomic<juce::int64> renderedStart { 0 };

    struct Section { juce::int64 start = 0; double rate = 44100.0; std::vector<float> transfer, rendered; juce::String json; };
    std::vector<Section> others;                          // the sections not being edited (guarded by transferLock + renderLock)
    bool newSectionPending = false;

    mutable juce::CriticalSection projectLock, transferLock;
    juce::String projectJson, fxJson;

    // ---- live effects (run after the tuned playback, so GarageBand hears the finished vocal)
    sudzfx::Chain fx;
    std::atomic<double> tailSeconds { 0.0 };
    juce::AudioParameterFloat* pReverb = nullptr;
    juce::AudioParameterFloat* pEcho = nullptr;
    juce::AudioParameterFloat* pDouble = nullptr;
    juce::AudioParameterFloat* pOutput = nullptr;
    bool lastArmed = false, lastRec = false;

    static constexpr int maxTransferSeconds = 15 * 60;

    std::atomic<double> hostBpm { 0.0 };
    int instanceId = 0;
    juce::CriticalSection nameLock; juce::String trackName;
    /** This instance's sections as one timeline-placed buffer (tuned where available). */
    bool compositeAudio (std::vector<float>& out, juce::int64& start, double& rate) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SudzProcessor)
};
