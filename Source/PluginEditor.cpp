#include "PluginEditor.h"
#include "BinaryData.h"
#include "NativeAI.h"

#if JUCE_MAC
void sudzCloseHostWindow (void* viewHandle);   // MacHelpers.mm
#endif

namespace
{
    std::vector<std::byte> toBytes (const void* data, size_t size)
    {
        std::vector<std::byte> v (size);
        std::memcpy (v.data(), data, size);
        return v;
    }
}

juce::File SudzEditor::splitModelFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Sudz").getChildFile ("models").getChildFile ("htdemucs_ft_vocals.onnx");
}

// AI work runs on the engine's own thread; the answer goes back to the page on the message thread, if the window is still open
void SudzEditor::runAiJob (juce::WebBrowserComponent::NativeFunctionCompletion complete, std::function<juce::var()> job)
{
    juce::Component::SafePointer<SudzEditor> safe (this);
    sudzai::Engine::get().pool().addJob ([safe, complete, job]
    {
        juce::var r;
        try { r = job(); }
        catch (const std::exception& e) { auto* o = new juce::DynamicObject(); o->setProperty ("error", juce::String (e.what())); r = juce::var (o); }
        catch (...) { auto* o = new juce::DynamicObject(); o->setProperty ("error", "the AI engine failed"); r = juce::var (o); }
        juce::MessageManager::callAsync ([safe, complete, r] { if (safe != nullptr) complete (r); });
    });
}

juce::WebBrowserComponent::Options SudzEditor::makeOptions()
{
    using Opts = juce::WebBrowserComponent::Options;
    return Opts{}
        .withBackend (Opts::Backend::webview2)
        .withWinWebView2Options (Opts::WinWebView2{}
                                    .withUserDataFolder (juce::File::getSpecialLocation (juce::File::tempDirectory)))
        .withNativeIntegrationEnabled()
        // the standalone Sudz app runs the full editor (open, save, harmonies, AI) instead of the GarageBand view
        .withInitialisationData ("standalone", proc.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
        .withKeepPageLoadedWhenBrowserIsHidden()
        .withResourceProvider ([this] (const juce::String& url) { return getResource (url); })
        .withNativeFunction ("pwGetState", [this] (const juce::Array<juce::var>&, auto complete)
        {
            complete (stateObject());
        })
        .withNativeFunction ("pwArm", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            proc.setArmed (args.size() > 0 && (bool) args[0], args.size() > 1 && args[1].toString() == "new");
            complete (true);
        })
        .withNativeFunction ("pwSaveProject", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            if (args.size() > 0) proc.setProjectJson (args[0].toString());
            complete (true);
        })
        .withNativeFunction ("pwPutRendered", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            if (args.size() >= 2)
            {
                juce::MemoryOutputStream decoded;
                if (juce::Base64::convertFromBase64 (decoded, args[1].toString()))
                {
                    const auto numSamples = (int) (decoded.getDataSize() / sizeof (float));
                    proc.writeRendered ((int) args[0], static_cast<const float*> (decoded.getData()), numSamples);
                    complete (numSamples);
                    return;
                }
            }
            complete (false);
        })
        .withNativeFunction ("pwSetSize", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            if (args.size() >= 2) setSize (juce::jlimit (640, 3000, (int) args[0]), juce::jlimit (460, 2000, (int) args[1]));
            complete (true);
        })
        .withNativeFunction ("pwCloseWindow", [this] (const juce::Array<juce::var>&, auto complete)
        {
           #if JUCE_MAC
            if (auto* peer = getPeer()) sudzCloseHostWindow (peer->getNativeHandle());
           #endif
            complete (true);
        })
        .withNativeFunction ("pwSaveChunk", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            // args: folder name, file name, byte offset, base64 data. Files go to ~/Music/Sudz/<folder>/
            if (args.size() < 4) { complete (juce::var()); return; }
            const auto safe = [] (juce::String n) { return juce::File::createLegalFileName (n.trim()).substring (0, 120); };
            const auto dir = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Sudz").getChildFile (safe (args[0].toString()));
            dir.createDirectory();
            const auto file = dir.getChildFile (safe (args[1].toString()));
            juce::MemoryOutputStream decoded;
            if (! juce::Base64::convertFromBase64 (decoded, args[3].toString())) { complete (juce::var()); return; }
            const auto offset = (juce::int64) (double) args[2];
            if (offset == 0) file.deleteFile();
            juce::FileOutputStream out (file);
            if (! out.openedOk()) { complete (juce::var()); return; }
            out.setPosition (offset);
            out.write (decoded.getData(), decoded.getDataSize());
            out.flush();
            complete (file.getFullPathName());
        })
        // small settings files shared by the plug-in and the Sudz app (singer presets): ~/Library/Application Support/Sudz/<name>.json
        .withNativeFunction ("pwLoadPrefs", [] (const juce::Array<juce::var>& args, auto complete)
        {
            const auto name = juce::File::createLegalFileName (args.size() > 0 ? args[0].toString() : juce::String ("prefs")).substring (0, 60);
            const auto f = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Sudz").getChildFile (name + ".json");
            complete (f.existsAsFile() ? juce::var (f.loadFileAsString()) : juce::var());
        })
        .withNativeFunction ("pwSavePrefs", [] (const juce::Array<juce::var>& args, auto complete)
        {
            if (args.size() < 2) { complete (false); return; }
            const auto name = juce::File::createLegalFileName (args[0].toString()).substring (0, 60);
            const auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Sudz");
            dir.createDirectory();
            complete (dir.getChildFile (name + ".json").replaceWithText (args[1].toString().substring (0, 4000000)));
        })
        // other tracks with Sudz on them (to line a double or harmony up with the lead)
        .withNativeFunction ("pwListPeers", [this] (const juce::Array<juce::var>&, auto complete)
        {
            juce::Array<juce::var> list;
            for (const auto& q : SudzProcessor::listPeers (&proc))
            {
                auto* o = new juce::DynamicObject();
                o->setProperty ("id", q.id); o->setProperty ("name", q.name); o->setProperty ("start", (double) q.start);
                o->setProperty ("length", (double) q.length); o->setProperty ("rate", q.rate);
                list.add (juce::var (o));
            }
            auto* me = new juce::DynamicObject(); me->setProperty ("peers", list); me->setProperty ("name", proc.getTrackName()); me->setProperty ("id", proc.getInstanceId());
            complete (juce::var (me));
        })
        // ---- native AI (ONNX Runtime on this Mac's processor); the page falls back to its own engine without it
        .withNativeFunction ("pwAiInfo", [] (const juce::Array<juce::var>&, auto complete)
        {
            auto& ai = sudzai::Engine::get();
            auto* o = new juce::DynamicObject();
            o->setProperty ("available", ai.available()); o->setProperty ("status", ai.status()); o->setProperty ("version", ai.version());
            o->setProperty ("threads", ai.threads());
            const auto split = splitModelFile();
            o->setProperty ("split", split.existsAsFile()); o->setProperty ("splitMB", (double) split.getSize() / 1e6);
            complete (juce::var (o));
        })
        .withNativeFunction ("pwAiLoad", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            const auto which = args.size() > 0 ? args[0].toString() : juce::String ("dfn");
            runAiJob (complete, [which]() -> juce::var
            {
                auto& ai = sudzai::Engine::get();
                auto* o = new juce::DynamicObject(); juce::var res (o);
                juce::String err; bool ok = ai.available();
                if (! ok) err = ai.status();
                else if (which == "dfn")
                {
                    for (auto n : { "enc", "erb_dec", "df_dec" })
                    {
                        if (ai.has (n)) continue;
                        int size = 0; const char* data = nullptr;
                        const auto want = juce::String (n) + ".onnx.b64.txt";
                        for (int i = 0; i < BinaryData::namedResourceListSize && data == nullptr; ++i)
                            if (want == BinaryData::originalFilenames[i]) data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size);
                        juce::MemoryOutputStream model;
                        if (data == nullptr || ! juce::Base64::convertFromBase64 (model, juce::String::fromUTF8 (data, size).trim())) { ok = false; err = "the clean-up network is missing from Sudz"; break; }
                        if (! ai.loadMemory (n, model.getData(), model.getDataSize(), false, err)) { ok = false; break; }
                    }
                }
                else if (which == "split")
                {
                    const auto f = splitModelFile();
                    if (! f.existsAsFile()) { ok = false; err = "the separation model isn't installed"; }
                    else if (! ai.has ("split")) ok = ai.loadFile ("split", f, false, err);
                }
                else { ok = false; err = "unknown network"; }
                o->setProperty ("ok", ok); if (! ok) o->setProperty ("error", err);
                return res;
            });
        })
        .withNativeFunction ("pwAiRun", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            // args: chain ("enc,erb_dec,df_dec"), JSON { inputs: { name: { dims, b64 } }, want: [names], pick: { name: [offset, count] } }
            const auto chainCsv = args.size() > 0 ? args[0].toString() : juce::String(), payload = args.size() > 1 ? args[1].toString() : juce::String();
            runAiJob (complete, [chainCsv, payload]() -> juce::var
            {
                auto* o = new juce::DynamicObject(); juce::var res (o);
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                const auto js = juce::JSON::parse (payload);
                sudzai::TensorMap in, out;
                if (auto* ins = js["inputs"].getDynamicObject())
                    for (const auto& kv : ins->getProperties())
                    {
                        auto& t = in[kv.name.toString().toStdString()];
                        if (auto* d = kv.value["dims"].getArray()) for (const auto& v : *d) t.dims.push_back ((int64_t) (double) v);
                        juce::MemoryOutputStream raw;
                        juce::Base64::convertFromBase64 (raw, kv.value["b64"].toString());
                        t.data.resize (raw.getDataSize() / sizeof (float));
                        std::memcpy (t.data.data(), raw.getData(), t.data.size() * sizeof (float));
                    }
                std::vector<std::string> chain, want;
                for (const auto& s : juce::StringArray::fromTokens (chainCsv, ",", "")) chain.push_back (s.trim().toStdString());
                if (auto* w = js["want"].getArray()) for (const auto& v : *w) want.push_back (v.toString().toStdString());
                juce::String err;
                if (! sudzai::Engine::get().runChain (chain, in, want, out, err)) { o->setProperty ("error", err); return res; }
                auto* outs = new juce::DynamicObject();
                for (const auto& kv : out)
                {
                    const auto& t = kv.second;
                    size_t a = 0, n = t.data.size();
                    if (auto pk = js["pick"][juce::Identifier (juce::String (kv.first))]; pk.isArray() && pk.size() == 2)
                    { a = (size_t) juce::jlimit (0.0, (double) n, (double) pk[0]); n = (size_t) juce::jlimit (0.0, (double) (t.data.size() - a), (double) pk[1]); }
                    auto* to = new juce::DynamicObject();
                    juce::Array<juce::var> dims; for (auto d : t.dims) dims.add ((double) d);
                    to->setProperty ("dims", dims);
                    to->setProperty ("b64", juce::Base64::toBase64 (t.data.data() + a, n * sizeof (float)));
                    outs->setProperty (juce::Identifier (juce::String (kv.first)), juce::var (to));
                }
                o->setProperty ("outputs", juce::var (outs));
                o->setProperty ("ms", juce::Time::getMillisecondCounterHiRes() - t0);
                return res;
            });
        })
        .withNativeFunction ("pwAiInstallSplit", [this] (const juce::Array<juce::var>&, auto complete)
        {
            // the 166 MB separation model, copied once into ~/Library/Application Support/Sudz/models
            chooser = std::make_unique<juce::FileChooser> ("Choose the separation model (htdemucs_ft_vocals_fp16weights.onnx)",
                                                           juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads"), "*.onnx");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [complete] (const juce::FileChooser& fc)
                                  {
                                      const auto f = fc.getResult();
                                      auto* o = new juce::DynamicObject(); juce::var res (o);
                                      if (! f.existsAsFile()) { complete (juce::var()); return; }
                                      if (f.getSize() < 100000000) { o->setProperty ("error", "That doesn't look like the separation model (it should be about 166 MB)."); complete (res); return; }
                                      const auto dst = splitModelFile();
                                      dst.getParentDirectory().createDirectory();
                                      if (! f.copyFileTo (dst)) { o->setProperty ("error", "The model couldn't be copied into Sudz's folder."); complete (res); return; }
                                      o->setProperty ("ok", true); o->setProperty ("mb", (double) dst.getSize() / 1e6);
                                      complete (res);
                                  });
        })
        .withNativeFunction ("pwReveal", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            if (args.size() > 0) { const juce::File f (args[0].toString()); if (f.exists()) f.revealToUser(); }
            complete (true);
        })
        .withNativeFunction ("pwSelectSection", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            complete (args.size() > 0 && proc.selectSection ((int) args[0]));
        })
        .withNativeFunction ("pwDeleteSection", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            complete (args.size() > 0 && proc.deleteSection ((int) args[0]));
        })
        .withNativeFunction ("pwOpenFile", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            // accept: the page's file-input filter, e.g. "audio/*,.wav,.mp3"
            juce::StringArray pats;
            for (auto p : juce::StringArray::fromTokens (args.size() > 0 ? args[0].toString() : juce::String(), ",", ""))
            {
                p = p.trim();
                if (p.startsWith (".")) pats.add ("*" + p);
                else if (p == "audio/*") pats.addArray ({ "*.wav", "*.aif", "*.aiff", "*.mp3", "*.m4a", "*.flac", "*.ogg", "*.caf" });
            }
            chooser = std::make_unique<juce::FileChooser> ("Open", juce::File::getSpecialLocation (juce::File::userMusicDirectory), pats.joinIntoString (";"));
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this, complete] (const juce::FileChooser& fc)
                                  {
                                      const auto f = fc.getResult();
                                      openedBytes.reset();
                                      if (! f.existsAsFile() || ! f.loadFileAsData (openedBytes)) { complete (juce::var()); return; }
                                      auto* o = new juce::DynamicObject();
                                      o->setProperty ("name", f.getFileName()); o->setProperty ("size", (double) openedBytes.getSize());
                                      complete (juce::var (o));
                                  });
        })
        .withNativeFunction ("pwGetVersion", [this] (const juce::Array<juce::var>&, auto complete)
        {
            complete (juce::String (JucePlugin_VersionString));
        })
        .withNativeFunction ("pwSetFx", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            if (args.size() > 0) proc.setFxJson (args[0].toString());
            complete (true);
        })
        .withNativeFunction ("pwPlayOriginal", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            proc.setPlayOriginal (args.size() > 0 && (bool) args[0]);
            complete (true);
        });
}

SudzEditor::SudzEditor (SudzProcessor& p)
    : AudioProcessorEditor (p), proc (p), web (makeOptions())
{
    addAndMakeVisible (web);
    web.goToURL (juce::WebBrowserComponent::getResourceProviderRoot());

    proc.onTransferStateChanged = [this]
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("armed", proc.isArmed());
        o->setProperty ("recording", proc.isRecording());
        o->setProperty ("seconds", (double) proc.getTransferLength() / proc.getHostSampleRate());
        o->setProperty ("timeline", proc.hostReportsTimeline());
        web.emitEventIfBrowserIsVisible ("pwTransferState", juce::var (o));
    };
    proc.onTransferFinished = [this]
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("length", proc.getTransferLength());
        o->setProperty ("sampleRate", proc.getTransferSampleRate());
        web.emitEventIfBrowserIsVisible ("pwTransferDone", juce::var (o));
    };

    // fit the screen: on a 13" MacBook Air a fixed 1280 x 820 window runs under the menu bar,
    // hiding the window's close button and Sudz's Transfer bar
    setResizable (true, true);
    setResizeLimits (640, 460, 3000, 2000);
    int w = 1040, h = 640;
    if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        const auto area = d->userArea;   // the screen minus the menu bar and Dock
        w = juce::jlimit (640, 1040, area.getWidth() - 200);
        h = juce::jlimit (460, 640, area.getHeight() - 240);   // room for GarageBand's own title bar and footer
    }
    setSize (w, h);
    startTimerHz (30);
}

SudzEditor::~SudzEditor()
{
    proc.onTransferStateChanged = nullptr;
    proc.onTransferFinished = nullptr;
    stopTimer();
}

void SudzEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff121417));
}

void SudzEditor::resized()
{
    web.setBounds (getLocalBounds());
}

void SudzEditor::timerCallback()
{
    const double t = proc.getPlayheadSeconds();
    const bool playing = proc.isHostPlaying();
    if (playing || playing != lastPlaying || std::abs (t - lastSentPlayhead) > 1e-3)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("t", t);
        o->setProperty ("playing", playing);
        web.emitEventIfBrowserIsVisible ("pwPlayhead", juce::var (o));
        lastSentPlayhead = t; lastPlaying = playing;
    }
}

juce::var SudzEditor::stateObject() const
{
    auto* o = new juce::DynamicObject();
    const bool has = proc.hasTransfer();
    o->setProperty ("hasTransfer", has);
    o->setProperty ("sampleRate", has ? proc.getTransferSampleRate() : proc.getHostSampleRate());
    o->setProperty ("length", proc.getTransferLength());
    o->setProperty ("project", proc.getProjectJson());
    o->setProperty ("armed", proc.isArmed());
    o->setProperty ("timeline", proc.hostReportsTimeline());
    o->setProperty ("start", (double) proc.getTransferStart());     // where the transfer begins on GarageBand's timeline (samples)
    o->setProperty ("bpm", proc.getHostBpm());                       // GarageBand's tempo (0 if not reported yet)
    juce::Array<juce::var> secs;
    for (const auto& sct : proc.getSections())
    {
        auto* so = new juce::DynamicObject();
        so->setProperty ("start", (double) sct.start); so->setProperty ("length", sct.length); so->setProperty ("active", sct.active);
        secs.add (juce::var (so));
    }
    o->setProperty ("sections", secs);
    o->setProperty ("standalone", proc.wrapperType == juce::AudioProcessor::wrapperType_Standalone);
    return juce::var (o);
}

std::optional<juce::WebBrowserComponent::Resource> SudzEditor::getResource (const juce::String& url)
{
    const auto path = url.upToFirstOccurrenceOf ("?", false, false);
    if (path == "/" || path == "/index.html")
        return juce::WebBrowserComponent::Resource { toBytes (BinaryData::index_html, (size_t) BinaryData::index_htmlSize), "text/html; charset=utf-8" };
    if (path == "/open/current")
        return juce::WebBrowserComponent::Resource { toBytes (openedBytes.getData(), openedBytes.getSize()), "application/octet-stream" };
    if (path.startsWith ("/ai/"))
    {
        // AI clean-up engine and model, compiled into the plug-in so it works offline
        const auto file = path.fromLastOccurrenceOf ("/", false, false);
        for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
            if (file == BinaryData::originalFilenames[i])
            {
                int size = 0;
                const auto* data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size);
                const auto mime = file.endsWith (".wasm") ? "application/wasm" : file.endsWith (".js") || file.endsWith (".mjs") ? "text/javascript" : "text/plain";
                return juce::WebBrowserComponent::Resource { toBytes (data, (size_t) size), mime };
            }
        return std::nullopt;
    }
    if (path.startsWith ("/peer/"))
    {
        const auto wav = SudzProcessor::peerAudioAsWav (path.fromLastOccurrenceOf ("/", false, false).upToFirstOccurrenceOf (".", false, false).getIntValue());
        if (wav.getSize() == 0) return std::nullopt;
        return juce::WebBrowserComponent::Resource { toBytes (wav.getData(), wav.getSize()), "audio/wav" };
    }
    if (path == "/transfer.wav")
    {
        const auto wav = proc.transferAsWav();
        return juce::WebBrowserComponent::Resource { toBytes (wav.getData(), wav.getSize()), "audio/wav" };
    }
    return std::nullopt;
}
