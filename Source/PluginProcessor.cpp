#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    // every Sudz instance in this process (one per track it's on)
    juce::CriticalSection& registryLock() { static juce::CriticalSection c; return c; }
    std::vector<SudzProcessor*>& registry() { static std::vector<SudzProcessor*> r; return r; }
    std::atomic<int> nextInstanceId { 1 };
}

std::vector<SudzProcessor::PeerInfo> SudzProcessor::listPeers (const SudzProcessor* except)
{
    std::vector<PeerInfo> v;
    const juce::ScopedLock rl (registryLock());
    for (auto* p : registry())
    {
        if (p == except) continue;
        const juce::ScopedLock sl (p->transferLock);
        juce::int64 a = std::numeric_limits<juce::int64>::max(), b = std::numeric_limits<juce::int64>::min();
        auto take = [&] (juce::int64 s, size_t n) { if (n == 0) return; a = std::min (a, s); b = std::max (b, s + (juce::int64) n); };
        take (p->transferStart, p->transfer.size());
        for (const auto& sct : p->others) take (sct.start, sct.transfer.size());
        if (b <= a) continue;
        v.push_back ({ p->instanceId, p->getTrackName(), a, b - a, p->transferRate });
    }
    return v;
}

bool SudzProcessor::compositeAudio (std::vector<float>& out, juce::int64& start, double& rate) const
{
    const juce::ScopedLock sl (transferLock);
    juce::int64 a = std::numeric_limits<juce::int64>::max(), b = std::numeric_limits<juce::int64>::min();
    auto span = [&] (juce::int64 s, size_t n) { if (n == 0) return; a = std::min (a, s); b = std::max (b, s + (juce::int64) n); };
    span (transferStart, transfer.size());
    for (const auto& sct : others) span (sct.start, sct.transfer.size());
    if (b <= a || b - a > (juce::int64) (maxTransferSeconds * 200000.0)) return false;
    out.assign ((size_t) (b - a), 0.0f); start = a; rate = transferRate;
    const juce::SpinLock::ScopedLockType lock (renderLock);
    auto place = [&] (const std::vector<float>& raw, const std::vector<float>& tuned, juce::int64 s)
    {
        const auto& src = tuned.size() == raw.size() ? tuned : raw;
        std::copy (src.begin(), src.end(), out.begin() + (s - a));
    };
    for (const auto& sct : others) place (sct.transfer, sct.rendered, sct.start);
    if (! transfer.empty()) place (transfer, rendered, transferStart);
    return true;
}

juce::MemoryBlock SudzProcessor::peerAudioAsWav (int id)
{
    std::vector<float> a; juce::int64 start = 0; double rate = 44100.0; bool ok = false;
    {
        const juce::ScopedLock rl (registryLock());
        for (auto* p : registry()) if (p->instanceId == id) { ok = p->compositeAudio (a, start, rate); break; }
    }
    if (! ok) return {};
    juce::MemoryOutputStream out;
    const auto bytes = (juce::uint32) (a.size() * sizeof (float));
    out.write ("RIFF", 4); out.writeInt ((int) (36 + bytes)); out.write ("WAVE", 4);
    out.write ("fmt ", 4); out.writeInt (16); out.writeShort (3); out.writeShort (1);
    out.writeInt ((int) rate); out.writeInt ((int) rate * 4); out.writeShort (4); out.writeShort (32);
    out.write ("data", 4); out.writeInt ((int) bytes);
    out.write (a.data(), bytes);
    return out.getMemoryBlock();
}

SudzProcessor::SudzProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    instanceId = nextInstanceId++;
    { const juce::ScopedLock rl (registryLock()); registry().push_back (this); }
    fifoSamples.resize (fifoSize);
    fifoPositions.resize (fifoSize);

    // GarageBand automation lanes: rides on top of the style chosen in the editor
    using PF = juce::AudioParameterFloat;
    addParameter (pReverb = new PF (juce::ParameterID { "reverb", 1 }, "Reverb amount", juce::NormalisableRange<float> (0.0f, 200.0f, 1.0f), 100.0f,
                                    juce::AudioParameterFloatAttributes().withLabel ("%")));
    addParameter (pEcho = new PF (juce::ParameterID { "echo", 1 }, "Echo amount", juce::NormalisableRange<float> (0.0f, 200.0f, 1.0f), 100.0f,
                                  juce::AudioParameterFloatAttributes().withLabel ("%")));
    addParameter (pDouble = new PF (juce::ParameterID { "double", 1 }, "Doubler amount", juce::NormalisableRange<float> (0.0f, 200.0f, 1.0f), 100.0f,
                                    juce::AudioParameterFloatAttributes().withLabel ("%")));
    addParameter (pOutput = new PF (juce::ParameterID { "output", 1 }, "Vocal level", juce::NormalisableRange<float> (-24.0f, 12.0f, 0.1f), 0.0f,
                                    juce::AudioParameterFloatAttributes().withLabel ("dB")));

    // live effects start switched off until the editor sends its settings
    sudzfx::Settings off; off.on = false;
    fx.setSettings (off);
    startTimerHz (50);
}

SudzProcessor::~SudzProcessor()
{
    {
        const juce::ScopedLock rl (registryLock());
        auto& r = registry(); r.erase (std::remove (r.begin(), r.end(), this), r.end());
    }
    stopTimer();
}

bool SudzProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    // mono in, stereo out lets the reverb, doubler and echo spread across the stereo field on a mono track
    return in == out || (in == juce::AudioChannelSet::mono() && out == juce::AudioChannelSet::stereo());
}

void SudzProcessor::prepareToPlay (double sampleRate, int)
{
    currentRate = sampleRate;
    fx.prepare (sampleRate);
    const auto json = getFxJson();
    if (json.isNotEmpty()) setFxJson (json);
    else { sudzfx::Settings off; off.on = false; fx.setSettings (off); }
}

void SudzProcessor::setFxJson (const juce::String& json)
{
    { const juce::ScopedLock sl (projectLock); fxJson = json; }
    sudzfx::Settings s;
    const auto v = juce::JSON::parse (json);
    if (auto* o = v.getDynamicObject())
        for (const auto& p : o->getProperties())
        {
            const auto key = p.name.toString().toStdString();
            if (key == "on") s.on = (bool) p.value;
            else if (p.value.isString()) s.setString (key, p.value.toString().toStdString());
            else if (auto* arr = p.value.getArray())
            {
                std::vector<double> v; for (const auto& e : *arr) v.push_back ((double) e);
                s.setList (key, v);
            }
            else if (p.value.isDouble() || p.value.isInt() || p.value.isInt64()) s.set (key, (double) p.value);
        }
    else s.on = false;
    fx.setSettings (s);
    tailSeconds.store (s.on ? std::max (0.3, (double) s.size) * 1.4 + (s.echo > 0 ? 2.5 : 0.0) : 0.0);
}

//==============================================================================
void SudzProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int chIn = getTotalNumInputChannels(), chOut = getTotalNumOutputChannels();
    for (int c = chIn; c < chOut; ++c)
        buffer.clear (c, 0, n);

    // Where are we on GarageBand's timeline?
    bool playing = false, hasPos = false;
    juce::int64 pos = 0;
    double bpm = 120.0;
    if (auto* ph = getPlayHead())
        if (auto info = ph->getPosition())
        {
            playing = info->getIsPlaying();
            if (auto t = info->getTimeInSamples()) { pos = *t; hasPos = true; }
            if (auto b = info->getBpm()) { bpm = *b; if (bpm > 20 && bpm < 400) hostBpm.store (bpm); }
        }

    if (! hasPos)
    {
        // Host gives no sample position: count from the start of playback instead.
        if (playing && ! wasPlaying.load()) fallbackCounter = 0;
        pos = fallbackCounter;
        if (playing) fallbackCounter += n;
    }
    hostHasTimeline.store (hasPos);
    hostPlaying.store (playing);

    if (! playing && wasPlaying.load() && armed.load())
        stopSeen.store (true);
    wasPlaying.store (playing);

    // 1) Transfer: copy the (mono-mixed) input into the FIFO with its timeline position.
    if (armed.load() && playing && chIn > 0)
    {
        recordingNow.store (true);
        int s1, z1, s2, z2;
        fifo.prepareToWrite (n, s1, z1, s2, z2);
        auto write = [&] (int dst, int count, int srcOffset)
        {
            for (int i = 0; i < count; ++i)
            {
                float m = 0.0f;
                for (int c = 0; c < chIn; ++c) m += buffer.getSample (c, srcOffset + i);
                fifoSamples[(size_t) (dst + i)] = m / (float) chIn;
                fifoPositions[(size_t) (dst + i)] = pos + srcOffset + i;
            }
        };
        write (s1, z1, 0);
        write (s2, z2, z1);
        fifo.finishedWrite (z1 + z2);
    }
    else
    {
        recordingNow.store (false);
    }

    // 2) Playback: inside the transferred stretch, output the tuned audio instead of the input.
    const auto rs = renderedStart.load();
    playheadSeconds.store ((double) (pos - rs) / currentRate);

    if (playing && ! armed.load() && ! playOriginal.load())
    {
        const juce::SpinLock::ScopedLockType lock (renderLock);
        auto playStretch = [&] (const std::vector<float>& r, juce::int64 start)
        {
            const auto len = (juce::int64) r.size();
            if (len <= 0 || pos + n <= start || pos >= start + len) return;
            for (int i = 0; i < n; ++i)
            {
                const auto t = pos + i - start;
                if (t < 0 || t >= len) continue;
                const float v = r[(size_t) t];
                for (int c = 0; c < chOut; ++c)
                    buffer.setSample (c, i, v);
            }
        };
        for (const auto& sct : others) playStretch (sct.rendered, sct.start);
        playStretch (rendered, rs);                       // the one being edited wins where they overlap
    }

    // 3) Live effects on the finished vocal (the transfer above always records the dry input)
    if (chOut > 0)
    {
        if (chIn == 1 && chOut == 2) buffer.copyFrom (1, 0, buffer, 0, 0, n);   // mono track: same signal both sides
        sudzfx::Rides rides;
        rides.reverb = pReverb->get() / 100.0f; rides.echo = pEcho->get() / 100.0f;
        rides.dbl = pDouble->get() / 100.0f; rides.outDb = pOutput->get();
        float* L = buffer.getWritePointer (0);
        float* R = chOut > 1 ? buffer.getWritePointer (1) : L;
        fx.process (L, R, n, bpm, rides);
    }
}

//==============================================================================
void SudzProcessor::timerCallback()
{
    drainFifo();
    if (stopSeen.exchange (false))
        finishTransfer();

    const bool a = armed.load(), r = recordingNow.load();
    if (a != lastArmed || r != lastRec || r)
    {
        lastArmed = a; lastRec = r;
        if (onTransferStateChanged) onTransferStateChanged();
    }
}

void SudzProcessor::drainFifo()
{
    const int ready = fifo.getNumReady();
    if (ready <= 0) return;

    int s1, z1, s2, z2;
    fifo.prepareToRead (ready, s1, z1, s2, z2);
    const auto limit = (juce::int64) (maxTransferSeconds * currentRate);

    const juce::ScopedLock sl (transferLock);
    auto put = [&] (int idx)
    {
        const auto p = fifoPositions[(size_t) idx];
        const float v = fifoSamples[(size_t) idx];
        if (transfer.empty()) { transferStart = p; transferRate = currentRate; }
        auto off = p - transferStart;
        if (off < 0)
        {
            if (-off + (juce::int64) transfer.size() > limit) return;
            transfer.insert (transfer.begin(), (size_t) -off, 0.0f);
            transferStart = p; off = 0;
        }
        if (off >= limit) return;
        if (off >= (juce::int64) transfer.size())
            transfer.resize ((size_t) off + 1, 0.0f);
        transfer[(size_t) off] = v;
    };
    for (int i = 0; i < z1; ++i) put (s1 + i);
    for (int i = 0; i < z2; ++i) put (s2 + i);
    fifo.finishedRead (z1 + z2);
}

void SudzProcessor::finishTransfer()
{
    armed.store (false);
    drainFifo();
    {
        const juce::ScopedLock sl (transferLock);
        const juce::SpinLock::ScopedLockType lock (renderLock);
        rendered = transfer;                    // until the editor sends tuned audio, play the recording as it was
        renderedStart.store (transferStart);
    }
    { const juce::ScopedLock sl (projectLock); projectJson = {}; }
    if (onTransferFinished) onTransferFinished();
    if (onTransferStateChanged) onTransferStateChanged();
}

void SudzProcessor::setArmed (bool shouldArm, bool asNewSection)
{
    if (shouldArm == armed.load()) return;
    if (shouldArm)
    {
        drainFifo();
        if (asNewSection)
        {
            // keep the current section: it moves to the others and a new one is recorded
            const juce::ScopedLock sl (transferLock);
            if (! transfer.empty())
            {
                Section sct; sct.start = transferStart; sct.rate = transferRate; sct.transfer = transfer; sct.json = getProjectJson();
                { const juce::SpinLock::ScopedLockType lock (renderLock); sct.rendered = rendered; others.push_back (std::move (sct)); }
            }
        }
        // a plain Transfer replaces the section being edited
        clearTransfer();
        armed.store (true);
    }
    else
    {
        finishTransfer();
    }
    if (onTransferStateChanged) onTransferStateChanged();
}

std::vector<SudzProcessor::SectionInfo> SudzProcessor::getSections() const
{
    const juce::ScopedLock sl (transferLock);
    std::vector<SectionInfo> v;
    for (const auto& sct : others) v.push_back ({ sct.start, (int) sct.transfer.size(), false });
    if (! transfer.empty()) v.push_back ({ transferStart, (int) transfer.size(), true });
    std::stable_sort (v.begin(), v.end(), [] (const SectionInfo& a, const SectionInfo& b) { return a.start < b.start; });
    return v;
}

bool SudzProcessor::selectSection (int index)
{
    const auto list = getSections();
    if (index < 0 || index >= (int) list.size() || list[(size_t) index].active) return false;
    const auto want = list[(size_t) index].start;
    const juce::ScopedLock sl (transferLock);
    auto it = std::find_if (others.begin(), others.end(), [&] (const Section& sct) { return sct.start == want; });
    if (it == others.end()) return false;
    Section next = std::move (*it);
    {
        const juce::SpinLock::ScopedLockType lock (renderLock);
        others.erase (it);
        if (! transfer.empty())
        {
            Section cur; cur.start = transferStart; cur.rate = transferRate; cur.transfer = std::move (transfer); cur.rendered = std::move (rendered); cur.json = getProjectJson();
            others.push_back (std::move (cur));
        }
        transfer = std::move (next.transfer); rendered = std::move (next.rendered);
        if (rendered.size() != transfer.size()) rendered = transfer;
        transferStart = next.start; transferRate = next.rate; renderedStart.store (next.start);
    }
    setProjectJson (next.json);
    return true;
}

bool SudzProcessor::deleteSection (int index)
{
    const auto list = getSections();
    if (index < 0 || index >= (int) list.size()) return false;
    if (list[(size_t) index].active)
    {
        clearTransfer();
        const juce::ScopedLock sl (transferLock);
        if (! others.empty())
        {
            // edit the earliest remaining section next
            auto it = std::min_element (others.begin(), others.end(), [] (const Section& a, const Section& b) { return a.start < b.start; });
            Section next = std::move (*it);
            const juce::SpinLock::ScopedLockType lock (renderLock);
            others.erase (it);
            transfer = std::move (next.transfer); rendered = std::move (next.rendered);
            if (rendered.size() != transfer.size()) rendered = transfer;
            transferStart = next.start; transferRate = next.rate; renderedStart.store (next.start);
            setProjectJson (next.json);
        }
        return true;
    }
    const auto want = list[(size_t) index].start;
    const juce::ScopedLock sl (transferLock);
    const juce::SpinLock::ScopedLockType lock (renderLock);
    others.erase (std::remove_if (others.begin(), others.end(), [&] (const Section& sct) { return sct.start == want; }), others.end());
    return true;
}

bool SudzProcessor::hasTransfer() const
{
    const juce::ScopedLock sl (transferLock);
    return ! transfer.empty();
}

void SudzProcessor::clearTransfer()
{
    const juce::ScopedLock sl (transferLock);
    const juce::SpinLock::ScopedLockType lock (renderLock);
    transfer.clear(); transfer.shrink_to_fit();
    rendered.clear(); rendered.shrink_to_fit();
    transferStart = 0; renderedStart.store (0);
    const juce::ScopedLock sl2 (projectLock);
    projectJson = {};
}

void SudzProcessor::writeRendered (int offset, const float* samples, int numSamples)
{
    const juce::ScopedLock sl (transferLock);
    const juce::SpinLock::ScopedLockType lock (renderLock);
    if (rendered.size() != transfer.size())
        rendered = transfer;
    if (offset < 0 || numSamples <= 0) return;
    const auto end = std::min ((size_t) offset + (size_t) numSamples, rendered.size());
    if ((size_t) offset >= end) return;
    std::memcpy (rendered.data() + offset, samples, (end - (size_t) offset) * sizeof (float));
}

juce::MemoryBlock SudzProcessor::transferAsWav() const
{
    const juce::ScopedLock sl (transferLock);
    juce::MemoryOutputStream out;
    const auto bytes = (juce::uint32) (transfer.size() * sizeof (float));
    out.write ("RIFF", 4); out.writeInt ((int) (36 + bytes)); out.write ("WAVE", 4);
    out.write ("fmt ", 4); out.writeInt (16); out.writeShort (3); out.writeShort (1);
    out.writeInt ((int) transferRate); out.writeInt ((int) transferRate * 4); out.writeShort (4); out.writeShort (32);
    out.write ("data", 4); out.writeInt ((int) bytes);
    out.write (transfer.data(), bytes);
    return out.getMemoryBlock();
}

//==============================================================================
// State: audio packed as 24-bit with headroom (tuned audio can rise above full scale after clip repair).
namespace
{
    constexpr int stateMagic = 0x50575231; // "PWR1"
    constexpr float transferScale = 0.5f, renderedScale = 0.25f;

    void writePacked (juce::MemoryOutputStream& out, const std::vector<float>& v, float scale)
    {
        out.writeInt ((int) v.size());
        juce::HeapBlock<juce::uint8> buf (v.size() * 3);
        for (size_t i = 0; i < v.size(); ++i)
        {
            const auto q = (int) std::lround (juce::jlimit (-1.0f, 1.0f, v[i] * scale) * 8388607.0f);
            buf[i * 3] = (juce::uint8) (q & 255); buf[i * 3 + 1] = (juce::uint8) ((q >> 8) & 255); buf[i * 3 + 2] = (juce::uint8) ((q >> 16) & 255);
        }
        out.write (buf.get(), v.size() * 3);
    }

    bool readPacked (juce::MemoryInputStream& in, std::vector<float>& v, float scale)
    {
        const int count = in.readInt();
        if (count < 0 || (juce::int64) count * 3 > in.getNumBytesRemaining()) return false;
        juce::HeapBlock<juce::uint8> buf ((size_t) count * 3);
        in.read (buf.get(), count * 3);
        v.resize ((size_t) count);
        for (size_t i = 0; i < (size_t) count; ++i)
        {
            int q = buf[i * 3] | (buf[i * 3 + 1] << 8) | (buf[i * 3 + 2] << 16);
            if (q & 0x800000) q -= 0x1000000;
            v[i] = (float) q / 8388607.0f / scale;
        }
        return true;
    }
}

void SudzProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::MemoryOutputStream out (destData, false);
    const juce::ScopedLock sl (transferLock);
    out.writeInt (stateMagic);
    out.writeInt (3);
    out.writeDouble (transferRate);
    out.writeInt64 (transferStart);
    out.writeString (getProjectJson());
    writePacked (out, transfer, transferScale);
    {
        const juce::SpinLock::ScopedLockType lock (renderLock);
        writePacked (out, rendered, renderedScale);
    }
    out.writeString (getFxJson());
    for (auto* p : { pReverb, pEcho, pDouble, pOutput }) out.writeFloat (p->get());
    // v3: the other sections
    out.writeInt ((int) others.size());
    for (const auto& sct : others)
    {
        out.writeDouble (sct.rate); out.writeInt64 (sct.start); out.writeString (sct.json);
        writePacked (out, sct.transfer, transferScale);
        const juce::SpinLock::ScopedLockType lock (renderLock);
        writePacked (out, sct.rendered, renderedScale);
    }
}

void SudzProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::MemoryInputStream in (data, (size_t) sizeInBytes, false);
    if (sizeInBytes < 16 || in.readInt() != stateMagic) return;
    const int version = in.readInt();
    if (version < 1 || version > 3) return;
    const double rate = in.readDouble();
    const auto start = in.readInt64();
    const auto json = in.readString();
    std::vector<float> t, r;
    if (! readPacked (in, t, transferScale)) return;
    if (! readPacked (in, r, renderedScale)) r = t;
    if (r.size() != t.size()) r = t;

    const juce::ScopedLock sl (transferLock);
    {
        const juce::SpinLock::ScopedLockType lock (renderLock);
        transfer = std::move (t);
        rendered = std::move (r);
        transferStart = start;
        renderedStart.store (start);
        transferRate = rate > 0 ? rate : 44100.0;
    }
    setProjectJson (json);

    if (version >= 2)
    {
        setFxJson (in.readString());
        for (auto* p : { pReverb, pEcho, pDouble, pOutput })
            if (in.getNumBytesRemaining() >= 4) *p = in.readFloat();
    }
    std::vector<Section> loaded;
    if (version >= 3 && in.getNumBytesRemaining() >= 4)
    {
        const int count = in.readInt();
        for (int i = 0; i < count && in.getNumBytesRemaining() > 0; ++i)
        {
            Section sct; sct.rate = in.readDouble(); sct.start = in.readInt64(); sct.json = in.readString();
            if (! readPacked (in, sct.transfer, transferScale)) break;
            if (! readPacked (in, sct.rendered, renderedScale) || sct.rendered.size() != sct.transfer.size()) sct.rendered = sct.transfer;
            loaded.push_back (std::move (sct));
        }
    }
    const juce::ScopedLock sl2 (transferLock);
    const juce::SpinLock::ScopedLockType lock (renderLock);
    others = std::move (loaded);
}

//==============================================================================
juce::AudioProcessorEditor* SudzProcessor::createEditor()
{
    return new SudzEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SudzProcessor();
}
