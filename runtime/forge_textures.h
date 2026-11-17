// Forge's opaque image IDs and descriptor retirement, independent of the renderer. The caller holds its lock.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

struct ForgeTextureImage { uint64_t view = 0, identity = 0; };

class ForgeTextures {
    struct Entry {
        uint64_t id;
        const void *texture;
        std::array<unsigned char, 40> view{};
        bool has_view, active = true;
        ForgeTextureImage image{};
        uint64_t descriptor = 0;
    };
    struct Retired { uint64_t descriptor, sequence; };
    std::vector<Entry> entries;
    std::vector<Retired> retired;
    uint32_t next = 1;
    static constexpr uint64_t tag = UINT64_C(0x464f524700000000);
public:
    static bool owns(uint64_t id) { return (id & UINT64_C(0xffffffff00000000)) == tag; }
    uint64_t add(const void *texture, const void *view, size_t view_size) {
        if (!texture || view_size > 40 || entries.size() >= 256 || !next) return 0;
        Entry e{};
        e.id = tag | next++;
        e.texture = texture;
        e.has_view = view != nullptr;
        if (view) std::memcpy(e.view.data(), view, view_size);
        entries.push_back(e);
        return e.id;
    }
    void remove(uint64_t id) {
        for (Entry &e : entries) if (e.id == id) e.active = false;
    }
    void forget(const void *texture) {
        for (Entry &e : entries) if (e.texture == texture) e.active = false;
    }
    uint64_t descriptor(uint64_t id) const {
        for (const Entry &e : entries) if (e.active && e.id == id) return e.descriptor;
        return 0;
    }
    // A view may be replaced by resolution/format changes. Keep its old descriptor through the pending GPU submission.
    template<class Resolve, class Create, class Destroy>
    void prepare(uint64_t pending, uint64_t completed, Resolve resolve, Create create, Destroy destroy) {
        for (const Retired &r : retired) if (r.sequence <= completed) destroy(r.descriptor);
        retired.erase(std::remove_if(retired.begin(), retired.end(), [completed](const Retired &r) {
            return r.sequence <= completed;
        }), retired.end());
        for (Entry &e : entries) {
            ForgeTextureImage image = e.active ? resolve(e.texture, e.has_view ? e.view.data() : nullptr) : ForgeTextureImage{};
            if (image.view == e.image.view && image.identity == e.image.identity && (e.descriptor || !image.view)) continue;
            if (e.descriptor) retired.push_back({e.descriptor, pending});
            e.image = image;
            e.descriptor = image.view ? create(image.view) : 0;
        }
        entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Entry &e) { return !e.active; }), entries.end());
    }
};
