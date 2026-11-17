#include "../runtime/forge_textures.h"
#include "../runtime/forge_texture_command.h"
#include <cassert>
#include <cstdio>
#include <vector>

int main() {
    ForgeTextures textures;
    int texture = 1, other = 2;
    unsigned char view[40] = {7};
    auto id = textures.add(&texture, view, sizeof view);
    assert(ForgeTextures::owns(id) && !ForgeTextures::owns(42));
    assert(textures.descriptor(id) == 0);
    ImDrawCmd command;
    command.TexRef = ImTextureRef((ImTextureID)id);
    command.ClipRect = ImVec4(1, 2, 90, 80);
    command.ElemCount = 6;
    ForgeTextureCommand patch;
    assert(ForgeTextureCommand::apply(textures, command, patch));
    assert(command.ClipRect.x == command.ClipRect.z && command.ClipRect.y == command.ClipRect.w);
    patch.restore();
    assert(command.GetTexID() == id && command.ClipRect.z == 90 && command.ElemCount == 6);
    view[0] = 99;
    uint64_t image = 11, image_identity = 1, serial = 100;
    int resolves = 0;
    std::vector<uint64_t> destroyed;
    auto resolve = [&](const void *t, const void *v) {
        assert(t == &texture);
        assert(v && *(const unsigned char *)v == 7);  // caller's view was copied
        ++resolves;
        return ForgeTextureImage{image, image_identity};
    };
    auto create = [&](uint64_t v) { assert(v == image); return ++serial; };
    auto destroy = [&](uint64_t descriptor) { destroyed.push_back(descriptor); };
    textures.prepare(4, 2, resolve, create, destroy);
    auto first = textures.descriptor(id);
    assert(first == 101 && resolves == 1);
    assert(ForgeTextureCommand::apply(textures, command, patch) && command.GetTexID() == 101);
    patch.restore();
    assert(command.GetTexID() == id);
    textures.prepare(5, 3, resolve, create, destroy);
    assert(textures.descriptor(id) == first && serial == 101 && destroyed.empty());

    ++image_identity;  // driver reused the same numeric VkImageView after cache eviction
    textures.prepare(6, 4, resolve, create, destroy);
    assert(textures.descriptor(id) == 102 && destroyed.empty());
    textures.prepare(7, 5, resolve, create, destroy);
    assert(destroyed.empty());
    textures.prepare(8, 6, resolve, create, destroy);
    assert(destroyed == std::vector<uint64_t>{first});

    textures.remove(id);
    textures.remove(UINT64_C(0x464f5247fffffff0));  // unknown IDs are harmless
    assert(textures.descriptor(id) == 0);
    assert(ForgeTextureCommand::apply(textures, command, patch));
    assert(command.ClipRect.z <= command.ClipRect.x);  // stale IDs are culled before Vulkan descriptor binding
    patch.restore();
    int before = resolves;
    textures.prepare(9, 7, resolve, create, destroy);
    assert(resolves == before && destroyed.size() == 1);
    textures.prepare(10, 8, resolve, create, destroy);
    assert(destroyed.size() == 1);
    textures.prepare(11, 9, resolve, create, destroy);
    assert(destroyed == (std::vector<uint64_t>{101, 102}));

    auto finalized = textures.add(&other, nullptr, 0);
    auto second_resolve = [&](const void *t, const void *v) {
        assert(t == &other && !v); ++resolves; return ForgeTextureImage{13, 3};
    };
    auto second_create = [&](uint64_t) { return UINT64_C(103); };
    textures.prepare(12, 10, second_resolve, second_create, destroy);
    assert(textures.descriptor(finalized) == 103);
    textures.forget(&other);  // nvnTextureFinalize removes the borrowed pointer before freeing it
    before = resolves;
    textures.prepare(13, 11, second_resolve, second_create, destroy);
    assert(resolves == before && textures.descriptor(finalized) == 0);
    textures.prepare(14, 13, second_resolve, second_create, destroy);
    assert(destroyed == (std::vector<uint64_t>{101, 102, 103}));
    auto next = textures.add(&texture, nullptr, 0);
    assert(next && next != id && next != finalized);  // stale snapshot IDs never alias a new texture

    ForgeTextures failed;
    auto retry = failed.add(&texture, nullptr, 0);
    auto nonzero = [](const void *, const void *) { return ForgeTextureImage{20, 4}; };
    auto no_descriptor = [](uint64_t) { return UINT64_C(0); };
    failed.prepare(1, 0, nonzero, no_descriptor, destroy);
    assert(failed.descriptor(retry) == 0);
    failed.prepare(2, 1, nonzero, second_create, destroy);
    assert(failed.descriptor(retry) == 103);  // a failed allocation retries, never exposes an NVN handle
    assert(failed.add(nullptr, nullptr, 0) == 0 && failed.add(&texture, view, 41) == 0);
    std::puts("Forge texture IDs, copied views, invalidation, descriptor retirement, retries: PASS");
}
