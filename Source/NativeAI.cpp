#include "NativeAI.h"
#include "ort/onnxruntime_c_api.h"
#include <dlfcn.h>
#include <thread>

namespace sudzai
{
namespace
{
    const OrtApi* A (const void* p) { return static_cast<const OrtApi*> (p); }

    // turns an OrtStatus into an error message (and frees it); true when there was no error
    bool check (const OrtApi* api, OrtStatus* st, juce::String& err)
    {
        if (st == nullptr) return true;
        err = juce::String (api->GetErrorMessage (st));
        api->ReleaseStatus (st);
        return false;
    }

    juce::File bundleFrameworks()
    {
        // for a plug-in this is the .component's own binary (JUCE finds it with dladdr), for the app its executable
        const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        return exe.getParentDirectory().getParentDirectory().getChildFile ("Frameworks");
    }
}

Engine& Engine::get()
{
    static Engine e;
    return e;
}

Engine::Engine()
{
    numThreads = juce::jlimit (1, 8, (int) std::thread::hardware_concurrency());
}

Engine::~Engine()
{
    workers.removeAllJobs (true, 2000);
    // Sessions and the environment are left for the process to reclaim: at exit ONNX Runtime may already have
    // torn down its own globals, and releasing into it then crashes. The library itself is never unloaded.
}

bool Engine::available()
{
    std::lock_guard<std::mutex> g (lock);
    if (tried) return ok;
    tried = true;

    const juce::String names[] = { "libonnxruntime.dylib", "libonnxruntime.1.19.2.dylib", "libonnxruntime.so" };
    const juce::File dirs[] = {
        bundleFrameworks(),
        juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Sudz").getChildFile ("lib")
    };
    juce::StringArray tried_;
    for (const auto& d : dirs)
        for (const auto& n : names)
        {
            const auto f = d.getChildFile (n);
            if (! f.existsAsFile()) continue;
            lib = dlopen (f.getFullPathName().toRawUTF8(), RTLD_NOW | RTLD_LOCAL);
            if (lib != nullptr) { libFile = f; break; }
            tried_.add (f.getFullPathName() + ": " + juce::String (dlerror()));
        }
    if (lib == nullptr)
    {
        statusText = tried_.isEmpty() ? "The AI engine library isn't installed (run install.command again)." : "The AI engine library wouldn't load: " + tried_.joinIntoString ("; ");
        return false;
    }
    using GetBase = const OrtApiBase* (*)();
    auto getBase = (GetBase) dlsym (lib, "OrtGetApiBase");
    if (getBase == nullptr) { statusText = "The AI engine library is not ONNX Runtime."; return false; }
    const auto* base = getBase();
    versionText = base->GetVersionString();
    const auto* a = base->GetApi (ORT_API_VERSION);
    if (a == nullptr) { statusText = "ONNX Runtime " + versionText + " is too old for Sudz (needs 1.19 or newer)."; return false; }
    api = a;
    OrtEnv* e = nullptr; juce::String err;
    if (! check (a, a->CreateEnv (ORT_LOGGING_LEVEL_ERROR, "sudz", &e), err)) { statusText = "ONNX Runtime didn't start: " + err; return false; }
    env = e;
    coremlAppend = dlsym (lib, "OrtSessionOptionsAppendExecutionProvider_CoreML");
    statusText = "ONNX Runtime " + versionText + " from " + libFile.getFullPathName();
    ok = true;
    return true;
}

juce::String Engine::status() const   { std::lock_guard<std::mutex> g (lock); return statusText; }
juce::String Engine::version() const  { std::lock_guard<std::mutex> g (lock); return versionText; }
juce::File Engine::libraryFile() const { std::lock_guard<std::mutex> g (lock); return libFile; }

bool Engine::has (const std::string& name) const
{
    std::lock_guard<std::mutex> g (lock);
    return sessions.count (name) > 0;
}

juce::String Engine::provider (const std::string& name) const
{
    std::lock_guard<std::mutex> g (lock);
    const auto it = sessions.find (name);
    return it == sessions.end() ? juce::String() : it->second.coreml ? "Core ML" : "CPU";
}

bool Engine::loadMemory (const std::string& name, const void* data, size_t size, bool tryCoreML, juce::String& err)
{
    return loadSession (name, data, size, nullptr, tryCoreML, err);
}

bool Engine::loadFile (const std::string& name, const juce::File& file, bool tryCoreML, juce::String& err)
{
    return loadSession (name, nullptr, 0, &file, tryCoreML, err);
}

bool Engine::loadSession (const std::string& name, const void* data, size_t size, const juce::File* file, bool tryCoreML, juce::String& err)
{
    if (! available()) { err = status(); return false; }
    std::lock_guard<std::mutex> g (lock);
    const auto* a = A (api);
    if (auto it = sessions.find (name); it != sessions.end()) { a->ReleaseSession ((OrtSession*) it->second.sess); sessions.erase (it); }

    auto create = [&] (bool coreml, OrtSession** out) -> bool
    {
        OrtSessionOptions* so = nullptr;
        if (! check (a, a->CreateSessionOptions (&so), err)) return false;
        a->SetIntraOpNumThreads (so, numThreads);
        a->SetInterOpNumThreads (so, 1);
        // big networks (the 166 MB separation model): no graph rewriting and no memory arena keep the peak
        // memory about 1 GB instead of 4.5 GB (measured; about 20% slower), which matters next to GarageBand
        const bool big = (file != nullptr ? file->getSize() : (juce::int64) size) > 50 * 1000 * 1000;
        const int lvl = juce::SystemStats::getEnvironmentVariable ("SUDZ_ORT_OPT", big ? "0" : "99").getIntValue();
        a->SetSessionGraphOptimizationLevel (so, lvl <= 0 ? ORT_DISABLE_ALL : lvl == 1 ? ORT_ENABLE_BASIC : lvl == 2 ? ORT_ENABLE_EXTENDED : ORT_ENABLE_ALL);
        if (big) a->DisableCpuMemArena (so);
        if (coreml)
        {
            using Append = OrtStatus* (*) (OrtSessionOptions*, uint32_t);
            if (coremlAppend == nullptr || ! check (a, ((Append) coremlAppend) (so, 0), err)) { a->ReleaseSessionOptions (so); return false; }
        }
        const bool made = file != nullptr ? check (a, a->CreateSession ((OrtEnv*) env, file->getFullPathName().toRawUTF8(), so, out), err)
                                          : check (a, a->CreateSessionFromArray ((OrtEnv*) env, data, size, so, out), err);
        a->ReleaseSessionOptions (so);
        return made;
    };

    Session s;
    OrtSession* sess = nullptr;
    if (tryCoreML && coremlAppend != nullptr && create (true, &sess)) s.coreml = true;
    else if (! create (false, &sess)) return false;
    s.sess = sess;

    OrtAllocator* alloc = nullptr;
    a->GetAllocatorWithDefaultOptions (&alloc);
    size_t ni = 0, no = 0;
    a->SessionGetInputCount (sess, &ni); a->SessionGetOutputCount (sess, &no);
    for (size_t i = 0; i < ni; ++i) { char* n = nullptr; if (a->SessionGetInputName (sess, i, alloc, &n) == nullptr && n != nullptr) { s.inputs.emplace_back (n); a->AllocatorFree (alloc, n); } }
    for (size_t i = 0; i < no; ++i) { char* n = nullptr; if (a->SessionGetOutputName (sess, i, alloc, &n) == nullptr && n != nullptr) { s.outputs.emplace_back (n); a->AllocatorFree (alloc, n); } }
    sessions[name] = std::move (s);
    return true;
}

bool Engine::runChain (const std::vector<std::string>& chain, const TensorMap& inputs,
                       const std::vector<std::string>& want, TensorMap& out, juce::String& err)
{
    if (! available()) { err = status(); return false; }
    const auto* a = A (api);

    // look the sessions up (they stay alive: they are only released when replaced, which the editor doesn't do mid-run)
    std::vector<Session> steps;
    {
        std::lock_guard<std::mutex> g (lock);
        for (const auto& n : chain)
        {
            const auto it = sessions.find (n);
            if (it == sessions.end()) { err = "the network " + juce::String (n) + " isn't loaded"; return false; }
            steps.push_back (it->second);
        }
    }

    OrtMemoryInfo* mem = nullptr;
    if (! check (a, a->CreateCpuMemoryInfo (OrtArenaAllocator, OrtMemTypeDefault, &mem), err)) return false;

    std::map<std::string, OrtValue*> pool;     // every tensor so far, by name
    std::vector<OrtValue*> owned;
    bool good = true;

    for (const auto& kv : inputs)
    {
        OrtValue* v = nullptr;
        auto& t = const_cast<Tensor&> (kv.second);
        size_t count = 1; for (auto d : t.dims) count *= (size_t) std::max<int64_t> (0, d);
        if (count != t.data.size()) { err = "input " + juce::String (kv.first) + " has the wrong size"; good = false; break; }
        if (! check (a, a->CreateTensorWithDataAsOrtValue (mem, t.data.data(), t.data.size() * sizeof (float), t.dims.data(), t.dims.size(),
                                                           ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &v), err)) { good = false; break; }
        owned.push_back (v); pool[kv.first] = v;
    }

    for (size_t si = 0; good && si < steps.size(); ++si)
    {
        const auto& s = steps[si];
        std::vector<const char*> inNames, outNames;
        std::vector<const OrtValue*> inVals;
        for (const auto& n : s.inputs)
        {
            const auto it = pool.find (n);
            if (it == pool.end()) { err = "the network " + juce::String (chain[si]) + " needs " + juce::String (n); good = false; break; }
            inNames.push_back (n.c_str()); inVals.push_back (it->second);
        }
        if (! good) break;
        for (const auto& n : s.outputs) outNames.push_back (n.c_str());
        std::vector<OrtValue*> outVals (outNames.size(), nullptr);
        if (! check (a, a->Run ((OrtSession*) s.sess, nullptr, inNames.data(), inVals.data(), inVals.size(),
                                outNames.data(), outNames.size(), outVals.data()), err)) { good = false; break; }
        for (size_t k = 0; k < outVals.size(); ++k) { owned.push_back (outVals[k]); pool[s.outputs[k]] = outVals[k]; }
    }

    if (good)
    {
        std::vector<std::string> names = want;
        if (names.empty() && ! steps.empty()) names = steps.back().outputs;
        for (const auto& n : names)
        {
            const auto it = pool.find (n);
            if (it == pool.end()) { err = "no output called " + juce::String (n); good = false; break; }
            OrtTensorTypeAndShapeInfo* info = nullptr;
            if (! check (a, a->GetTensorTypeAndShape (it->second, &info), err)) { good = false; break; }
            ONNXTensorElementDataType type {}; size_t nd = 0, count = 0;
            a->GetTensorElementType (info, &type); a->GetDimensionsCount (info, &nd); a->GetTensorShapeElementCount (info, &count);
            Tensor t; t.dims.resize (nd); a->GetDimensions (info, t.dims.data(), nd);
            a->ReleaseTensorTypeAndShapeInfo (info);
            if (type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) { err = "output " + juce::String (n) + " isn't float"; good = false; break; }
            float* p = nullptr;
            if (! check (a, a->GetTensorMutableData (it->second, (void**) &p), err)) { good = false; break; }
            t.data.assign (p, p + count);
            out[n] = std::move (t);
        }
    }

    for (auto* v : owned) a->ReleaseValue (v);
    a->ReleaseMemoryInfo (mem);
    return good;
}
} // namespace sudzai
