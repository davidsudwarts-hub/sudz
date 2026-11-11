#include "PluginEditor.h"
#include "BinaryData.h"

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
    if (path == "/transfer.wav")
    {
        const auto wav = proc.transferAsWav();
        return juce::WebBrowserComponent::Resource { toBytes (wav.getData(), wav.getSize()), "audio/wav" };
    }
    return std::nullopt;
}
