#pragma once

#include "document/composition.hpp"
#include "effects/effect_registry.hpp"

#include <Metal/Metal.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace azl {

// Render thread only. The targets of multi-pass effects, by effect id and side pass, so an effect keeps them wherever
// it moves. A persistent target (e.g. Trails history) is two textures that swap roles every frame, so its history is
// never copied. It lives as long as its effect stays in the document: edits of its chain keep it, a change of the
// clip the drawing layer plays clears those of the clip's effects but keeps the layer's own, and a change of the group
// it sits in clears them all, so the history restarts from the current picture. A layer's outgoing clip during a
// transition draws with a layer id of its own (the top bit set) and has targets of its own. Targets no longer used are
// kept as spares for a few seconds and reused by the next target of their size, so a clip change (a new clip's
// effects, the outgoing clip's fade) allocates no textures.
class SideTargets {
public:
    SideTargets(MTL::Device* device, uint32_t width, uint32_t height);

    struct Target {
        MTL::Texture* write = nullptr;
        // Last frame's content of a persistent target; it needs clearing first when `fresh`.
        MTL::Texture* prev = nullptr;
        bool fresh = false;
    };

    // Called once per frame for every drawn layer, before its side passes, with the layer's own chain.
    void use(uint64_t layer, const std::string& clip, uint64_t parent, const std::vector<Effect>& own = {});
    // Drops the targets of layers that are gone and of effects no longer in the document. Called once per frame.
    void keep(const std::set<uint64_t>& layers, const std::set<uint64_t>& effects);
    // The target side pass `side` of effect `effect` writes this frame, for a layer drawn at `width` x `height` (0 is
    // the composition size); a change of size or format restarts it.
    Target begin(uint64_t layer, uint64_t effect, size_t side, const SidePass& pass, bool float16, uint32_t width = 0,
                 uint32_t height = 0);
    // What that side pass wrote this frame.
    // Null before the side pass first ran.
    MTL::Texture* output(uint64_t layer, uint64_t effect, size_t side) const;
    size_t count() const { return targets_.size(); }
    size_t spares() const { return spares_.size(); }
    // Allocated bytes of the targets and spares.
    size_t bytes() const;
    // The effect's persistent targets start their history again the next time they are written.
    void restart(uint64_t layer, uint64_t effect);

private:
    struct Textures {
        NS::SharedPtr<MTL::Texture> texture[2];
        int written = 0;
        bool fresh = true;
        // The layer that last drew it.
        uint64_t layer = 0;
    };
    struct Layer {
        std::string clip;
        uint64_t parent = 0;
    };
    // Effect, side pass, outgoing.
    using Key = std::tuple<uint64_t, size_t, bool>;

    struct Spare {
        NS::SharedPtr<MTL::Texture> texture;
        uint64_t since = 0;
    };

    static Key key(uint64_t layer, uint64_t effect, size_t side);
    NS::SharedPtr<MTL::Texture> take(uint32_t width, uint32_t height, bool float16);
    void release(Textures& textures);

    NS::SharedPtr<MTL::Device> device_;
    uint32_t width_;
    uint32_t height_;
    std::map<uint64_t, Layer> layers_;
    std::map<Key, Textures> targets_;
    std::vector<Spare> spares_;
    uint64_t frame_ = 0;
};

} // namespace azl
