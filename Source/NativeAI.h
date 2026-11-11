#pragma once

#include <JuceHeader.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>

/*  Native AI for Sudz: the same networks the editor runs in the browser (DeepFilterNet3 clean-up, HT-Demucs
    vocal split), run on the Mac's own processor through ONNX Runtime, several times faster than WebAssembly.

    ONNX Runtime is loaded at run time (dlopen), never linked: if the library is missing or won't load on this
    Mac, Sudz still opens and the editor falls back to its in-browser engine. It is looked for in
      <bundle>/Contents/Frameworks/libonnxruntime.dylib
      ~/Library/Application Support/Sudz/lib/libonnxruntime.dylib   (where the installer puts it, shared by the app and the plug-in)

    One engine per process (GarageBand runs every Sudz instance in one process), shared by all instances. */
namespace sudzai
{
struct Tensor
{
    std::vector<int64_t> dims;
    std::vector<float> data;
};
using TensorMap = std::map<std::string, Tensor>;

class Engine
{
public:
    static Engine& get();

    /** Loads ONNX Runtime the first time; true when it is ready. */
    bool available();
    juce::String status() const;          // why it isn't available, or where it was loaded from
    juce::String version() const;
    juce::File libraryFile() const;
    int threads() const                    { return numThreads; }

    /** Creates a session called `name` from model bytes or a file. Core ML is tried first when asked, with the
        processor as the fallback (some networks have parts Core ML can't run). */
    bool loadMemory (const std::string& name, const void* data, size_t size, bool tryCoreML, juce::String& err);
    bool loadFile (const std::string& name, const juce::File& file, bool tryCoreML, juce::String& err);
    bool has (const std::string& name) const;
    juce::String provider (const std::string& name) const;   // "Core ML" or "CPU"

    /** Runs sessions one after another. Each takes the inputs it needs by name from a pool: the inputs given
        here plus everything the earlier sessions produced, so tensors passed between networks never leave
        native memory. Returns the tensors named in `want` (all outputs of the last session if empty). */
    bool runChain (const std::vector<std::string>& chain, const TensorMap& inputs,
                   const std::vector<std::string>& want, TensorMap& out, juce::String& err);

    /** One background thread for inference, so neither GarageBand's window nor the audio waits on it. */
    juce::ThreadPool& pool()               { return workers; }

    ~Engine();

private:
    Engine();
    bool loadSession (const std::string& name, const void* data, size_t size, const juce::File* file, bool tryCoreML, juce::String& err);

    mutable std::mutex lock;
    bool tried = false, ok = false;
    void* lib = nullptr;
    const void* api = nullptr;             // const OrtApi*
    void* env = nullptr;                   // OrtEnv*
    void* coremlAppend = nullptr;          // OrtSessionOptionsAppendExecutionProvider_CoreML, if the library has it
    juce::String statusText, versionText;
    juce::File libFile;
    int numThreads = 2;
    struct Session { void* sess = nullptr; std::vector<std::string> inputs, outputs; bool coreml = false; };
    std::map<std::string, Session> sessions;
    juce::ThreadPool workers { 1 };
};
} // namespace sudzai
