#pragma once

#include "document/composition.hpp"
#include "effects/chain_planner.hpp"

#include "gpu/gpu.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace azl {

// How a last pass reaches its target: one fixed-function blend per mode that has one, and the shader fallback that
// reads a copy of the target for every other mode.
enum class BlendVariant { Normal, Add, Screen, Multiply, Shader };
inline constexpr size_t kBlendVariants = 5;
// A last pass's pipelines: the kBlendVariants into the composition, then the same mixing in a transition's outgoing
// picture.
inline constexpr size_t kFadePipelines = kBlendVariants;
// Then, for a single-pass chain on a video source, the same evaluating the outgoing clip in the pass itself.
inline constexpr size_t kFadeSinglePipelines = 2 * kBlendVariants;

BlendVariant blendVariant(BlendMode mode);

struct CompiledPass {
    PlannedPass plan;
    // One pipeline, or for a last pass one per BlendVariant, so switching a blend mode never compiles.
    std::vector<gpu::RenderPipeline> pipelines;
};

// The effects one draw runs, in order, holding the parameter values the shaders read.
using EffectList = std::vector<const Effect*>;

// The sources a chain is compiled for: a layer's test card and clips; a clip's own chain fused with its layer's, for
// that clip only; a group's or the composition's sub-stack.
enum class ChainKind { Layer, Clip, Group };

// A clip's chain followed by its layer's chain in the clip's deck, which the planner fuses as one; without a clip the
// layer's chain alone.
EffectList clipChain(const std::vector<Effect>& layerEffects, const ClipRef* clip);
ChainKind chainKind(const ClipRef* clip);
EffectList effectList(const std::vector<Effect>& effects);
// A layer holding a text clip compiles its chains for text pictures too.
bool holdsText(const Layer& layer);

// Whether the device can keep a pipeline cache. Apple's Paravirtual device (a macOS VM, as on CI runners) traps inside
// Metal's compiler adding to an archive after an engine on it was killed (issue #29), so there it runs without one.
bool archivesWork(std::string_view deviceName);

// The pass's entry points and the fullscreen vertex shader as one Metal source, each function named
// mslFunction(entry). Throws with the compiler's error when the GLSL does not compile. Any thread.
std::string passMsl(const PlannedPass& pass);

// The stages' parameter values in the order the generated shader reads them.
void paramsOf(const PlannedPass& pass, const EffectList& effects, std::vector<float>& values);
// A bypassed effect's dry/wet, its last parameter, is 0 whatever drives it.
void bypass(const PlannedPass& pass, const EffectList& effects, std::vector<float>& values);
// Appends the stages' derived values after their parameters, at the header's time and beat. `rates` are the same
// parameters with each driven one's own value (ModulationPlan::rates).
void derive(const PlannedPass& pass, std::vector<float>& values, const std::vector<float>& rates,
            const PassHeader& header);

// One effect chain, compiled for every source its kind can show; for text only when asked.
struct CompiledChain {
    std::vector<CompiledPass> testCard;
    std::vector<CompiledPass> yuv;
    std::vector<CompiledPass> group;
    std::vector<CompiledPass> text{};
    // A chain starting with a generator draws only from it.
    std::vector<CompiledPass> generator{};
    // A layer chain with effects, for an alpha transition between two of its clips mixed at the source.
    std::vector<CompiledPass> yuvFade{};
};

struct CompileStats {
    // Pipelines compiled; a cache hit does not count.
    std::atomic<uint64_t> pipelines{0};
    // Pipelines the persistent cache held, built without compiling.
    std::atomic<uint64_t> archived{0};
    // The slowest pass, with all its blend variants.
    std::atomic<uint64_t> maxMicros{0};
};

// The effect type whose shader draws `effect`: its first variant with a parameter on, else its own.
std::string shaderType(const Composition& composition, const Effect& effect);

// Identifies a chain by its kind, its effects' shader types in order and whether it draws text; parameter values are
// uniforms and change it only by switching a variant.
std::string chainKey(const Composition& composition, ChainKind kind, const EffectList& effects, bool text = false);

// Compiles effect chains on its own thread. The render thread only looks finished chains up, so it never waits for a
// compile, even a slow or failing one.
class ChainCompiler {
public:
    // Called on the compile thread with the chain's effect types and the error.
    using Failure = std::function<void(const std::vector<std::string>& chain, const std::string& error)>;

    // With `cache`, compiled pipelines persist in that file, so a later compiler on the same file compiles none of
    // them again.
    explicit ChainCompiler(const gpu::Device& device, std::filesystem::path cache = {});
    ~ChainCompiler();

    // Any thread, returns at once: queues every chain of the composition that is not compiled yet (each layer's, each
    // clip's fused with its layer's, each group's and the composition's; for a transition each layer's on a sub-stack
    // and each clip's alone) and the chains with no effects, which a layer shows until it has a chain of its own (the
    // layer's one with text). A chain that failed is not retried.
    void prepare(const Composition& composition);
    // Any thread, returns at once: wants only this one chain, for a compiler of its own such as a preview's. Chains
    // compiled before stay ready.
    void prepare(const Composition& composition, ChainKind kind, const EffectList& effects);
    void onFailure(Failure failure);
    // Any thread: the next prepare compiles every chain again, as after the effects or ISF transitions changed, and
    // retries the failed ones; a chain compiled before is found until its new version is ready.
    void refresh();
    // Null until the chain is compiled.
    std::shared_ptr<const CompiledChain> find(const std::string& key) const;
    bool failed(const std::string& key) const;

    // For tests: a held compiler starts no compile until released.
    void hold(bool held);
    void waitIdle();
    // Nothing is queued or compiling. Never blocks.
    bool idle() const;
    CompileStats& stats() { return stats_; }
    // Chains compiled and wanted.
    std::pair<size_t, size_t> sizes() const;

private:
    struct Job {
        std::string key;
        std::vector<std::string> types;
        ChainKind kind = ChainKind::Layer;
        bool text = false;
    };

    // Under mutex_.
    void wantLocked(const Composition& composition, ChainKind kind, const EffectList& effects, bool text);
    void run();
    std::shared_ptr<const CompiledChain> compile(const Job& job);
    void fail(const std::vector<std::string>& types, const std::string& error);
    std::vector<CompiledPass> compilePasses(SourceKind source, const std::vector<const EffectDef*>& chain);
    std::vector<gpu::RenderPipeline> pipelines(const PlannedPass& pass);
    void openArchive();
    void saveArchive();
    void reportFirstIdle();

    gpu::Device device_;
    CompileStats stats_;
    std::filesystem::path cachePath_;
    // Compile thread only.
    gpu::PipelineCache archive_;
    bool archiveChanged_ = false;
    std::chrono::steady_clock::time_point created_ = std::chrono::steady_clock::now();
    bool reported_ = false;
    uint64_t compiled_ = 0;
    uint64_t cached_ = 0;
    // Compile thread only, by generated source and target.
    std::map<std::string, std::vector<gpu::RenderPipeline>> pipelines_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Job> queue_;
    std::set<std::string> wanted_;
    std::map<std::string, std::shared_ptr<const CompiledChain>> ready_;
    std::set<std::string> failed_;
    // The effect types of the failed chains reported, so a chain compiled for several sources reports once.
    std::set<std::vector<std::string>> reportedFailures_;
    // Ready chains to compile again.
    std::set<std::string> stale_;
    Failure onFailure_;
    bool busy_ = false;
    bool held_ = false;
    bool stopping_ = false;
    std::thread thread_;
};

} // namespace azl
