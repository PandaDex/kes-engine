#include "test_framework.h"
#include <cstring>
#include <filesystem>
#include <sstream>
#include "animation_clip.h"
#include "kmodel.h"
#include "sound_file.h"
#include "texture_cook.h"
#include "texture_file.h"

namespace
{
    using LoadFn = bool (*)(const std::string& path);

    void CheckRejectsCorruption(const kestest::TempDir& dir, const std::string& validPath, LoadFn load)
    {
        const std::vector<uint8_t> valid = kestest::ReadBytes(validPath);
        REQUIRE(!valid.empty());
        const std::string path = dir.File("corrupt.bin");

        for (size_t size = 0; size < valid.size(); ++size)
        {
            kestest::WriteBytes(path, valid.data(), size);
            if (load(path))
                kestest::ReportFailure(__FILE__, __LINE__, "accepted a file truncated to " + std::to_string(size) + " bytes");
        }

        std::vector<uint8_t> badMagic = valid;
        badMagic[0] ^= 0xFF;
        kestest::WriteBytes(path, badMagic);
        CHECK(!load(path));
    }

    KModelData MakeModel()
    {
        KModelData model;
        model.materials.push_back({ "stone", "materials/stone.kmat" });

        KModelMesh mesh;
        mesh.name = "triangle";
        mesh.vertices.resize(3);
        mesh.vertices[1].px = 1.0f;
        mesh.vertices[2].py = 2.0f;
        mesh.vertices[2].boneIndices[0] = 1;
        mesh.indices = { 0, 1, 2 };
        mesh.primitives.push_back({ 0, 3, 0 });
        model.meshes.push_back(mesh);

        KModelNode root;
        root.name = "root";
        root.meshIndex = 0;
        root.boneIndex = 0;
        root.translation[1] = 3.0f;
        model.nodes.push_back(root);

        KModelBone bone;
        bone.name = "spine";
        bone.nodeIndex = 0;
        model.bones.push_back(bone);

        KModelSocket socket;
        socket.name = "hand_r";
        socket.boneIndex = 0;
        socket.translation[2] = 0.25f;
        model.sockets.push_back(socket);

        KModelCollider collider;
        collider.name = "body";
        collider.shape = KModelColliderShape::Capsule;
        collider.boneIndex = 0;
        collider.radius = 0.3f;
        collider.halfHeight = 0.6f;
        model.colliders.push_back(collider);

        model.defaultMaterial = "materials/skin.mat";
        KModelChild child;
        child.name = "gun";
        child.modelPath = "models/pistol.kmodel";
        child.materialPath = "materials/gun.mat";
        child.animationPath = "anims/gun_idle.kanim";
        child.boneIndex = 0;
        child.translation[0] = 0.1f;
        child.scale[2] = 2.0f;
        model.children.push_back(child);
        return model;
    }

    SoundFileData MakeSound()
    {
        SoundFileData sound;
        sound.sourcePath = "tone.wav";
        sound.sampleRate = 48000;
        sound.channels = 2;
        sound.frameCount = 2;
        sound.samples = { 0.0f, 0.25f, -0.5f, 1.0f };
        return sound;
    }

    TextureFileData MakeTexture()
    {
        TextureFileData texture;
        texture.sourcePath = "checker.png";
        texture.width = 2;
        texture.height = 1;
        texture.flags = TextureFileFlag_Srgb | TextureFileFlag_ClampU;
        texture.pixels = { 255, 0, 0, 255, 0, 255, 0, 128 };
        return texture;
    }

    bool LoadModel(const std::string& path)
    {
        KModelData model;
        return LoadKModelFile(path.c_str(), model);
    }

    bool LoadSound(const std::string& path)
    {
        SoundFileData sound;
        return LoadKSoundFile(path.c_str(), sound);
    }

    bool LoadTexture(const std::string& path)
    {
        TextureFileData texture;
        return LoadKTexFile(path.c_str(), texture);
    }
}

TEST_CASE("assets: everything shipped passes the loaders' validation")
{
    namespace fs = std::filesystem;
    std::istringstream roots(KES_TEST_CONTENT_DIRS);
    int loaded = 0;
    for (std::string root; std::getline(roots, root, '|');)
    {
        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root))
        {
            const std::string path = entry.path().string();
            const std::string extension = entry.path().extension().string();
            std::string error;
            bool ok = true;
            if (extension == ".kmodel")
            {
                KModelData model;
                ok = LoadKModelFile(path.c_str(), model, &error);
            }
            else if (extension == ".ksound")
            {
                SoundFileData sound;
                ok = LoadKSoundFile(path.c_str(), sound, &error);
            }
            else if (extension == ".ktex")
            {
                TextureFileData texture;
                ok = LoadKTexFile(path.c_str(), texture, &error);
            }
            else if (extension == ".kanim")
            {
                AnimationClipData clip;
                ok = LoadAnimationClipFile(path.c_str(), clip, &error);
            }
            else
            {
                continue;
            }
            ++loaded;
            if (!ok)
                kestest::ReportFailure(__FILE__, __LINE__, path + ": " + error);
        }
    }
    CHECK(loaded > 0);
}

TEST_CASE("kmodel: broken references are rejected")
{
    kestest::TempDir dir;
    const std::string path = dir.File("model.kmodel");
    const auto rejects = [&](const KModelData& model)
    {
        REQUIRE(SaveKModelFile(path.c_str(), model));
        KModelData loaded;
        std::string error;
        return !LoadKModelFile(path.c_str(), loaded, &error) && !error.empty();
    };

    KModelData model = MakeModel();
    model.meshes[0].indices[0] = 3;
    CHECK(rejects(model));

    model = MakeModel();
    model.meshes[0].primitives[0].indexCount = 4;
    CHECK(rejects(model));

    model = MakeModel();
    model.meshes[0].primitives[0].materialIndex = 1;
    CHECK(rejects(model));

    model = MakeModel();
    model.nodes[0].parentIndex = 5;
    CHECK(rejects(model));

    model = MakeModel();
    model.nodes[0].parentIndex = 0;   // a cycle used to hang mesh building
    CHECK(rejects(model));

    model = MakeModel();
    model.bones[0].nodeIndex = 1;
    CHECK(rejects(model));
}

TEST_CASE("kmodel: a huge element count fails instead of allocating")
{
    kestest::TempDir dir;
    const std::string path = dir.File("model.kmodel");
    KModelData model = MakeModel();
    model.sourcePath.clear();
    REQUIRE(SaveKModelFile(path.c_str(), model));

    std::vector<uint8_t> bytes = kestest::ReadBytes(path);
    const uint32_t huge = 0xFFFFFFFFu;
    std::memcpy(bytes.data() + 12, &huge, sizeof(huge));   // magic, version, source path length, material count
    kestest::WriteBytes(path, bytes);
    CHECK(!LoadModel(path));
}

TEST_CASE("kanim: round trip keeps the clip and sampling interpolates")
{
    kestest::TempDir dir;
    const std::string path = dir.File("clip.kanim");

    AnimationClipData clip;
    clip.name = "walk";
    clip.duration = 2.0f;
    clip.looping = false;
    AnimationNodeTrack track;
    track.nodeName = "root";
    track.nodeIndex = 0;
    track.translationKeys = { { 0.0f, { 0.0f, 0.0f, 0.0f } }, { 1.0f, { 2.0f, 0.0f, 0.0f } }, { 2.0f, { 2.0f, 4.0f, 0.0f } } };
    track.rotationKeys = { { 0.0f, { 0.0f, 0.0f, 0.0f, 1.0f } } };
    clip.tracks.push_back(track);
    REQUIRE(SaveAnimationClipFile(path.c_str(), clip));

    AnimationClipData loaded;
    REQUIRE(LoadAnimationClipFile(path.c_str(), loaded));
    CHECK(loaded.name == "walk");
    CHECK(!loaded.looping);
    REQUIRE(loaded.tracks.size() == 1);
    REQUIRE(loaded.tracks[0].translationKeys.size() == 3);

    KModelData model = MakeModel();
    std::vector<AnimationTransform> pose;
    REQUIRE(SampleAnimationClip(model, loaded, 0.5f, pose));
    CHECK_NEAR(pose[0].translation[0], 1.0, 1e-5);
    REQUIRE(SampleAnimationClip(model, loaded, 1.5f, pose));
    CHECK_NEAR(pose[0].translation[1], 2.0, 1e-5);
    REQUIRE(SampleAnimationClip(model, loaded, 1.0f, pose));
    CHECK_NEAR(pose[0].translation[0], 2.0, 1e-5);

    const auto load = [](const std::string& file)
    {
        AnimationClipData result;
        return LoadAnimationClipFile(file.c_str(), result);
    };
    const std::vector<uint8_t> valid = kestest::ReadBytes(path);
    const std::string corrupt = dir.File("corrupt.kanim");
    for (size_t size = 0; size < valid.size(); ++size)
    {
        kestest::WriteBytes(corrupt, valid.data(), size);
        if (load(corrupt))
            kestest::ReportFailure(__FILE__, __LINE__, "accepted a clip truncated to " + std::to_string(size) + " bytes");
    }
}

TEST_CASE("kmodel: round trip keeps the model")
{
    kestest::TempDir dir;
    const std::string path = dir.File("model.kmodel");
    REQUIRE(SaveKModelFile(path.c_str(), MakeModel()));

    KModelData loaded;
    REQUIRE(LoadKModelFile(path.c_str(), loaded));

    REQUIRE(loaded.meshes.size() == 1);
    const KModelMesh& mesh = loaded.meshes[0];
    CHECK(mesh.name == "triangle");
    REQUIRE(mesh.vertices.size() == 3);
    CHECK_NEAR(mesh.vertices[1].px, 1.0, 1e-6);
    CHECK_NEAR(mesh.vertices[2].py, 2.0, 1e-6);
    CHECK(mesh.vertices[2].boneIndices[0] == 1);
    CHECK(mesh.indices == std::vector<uint32_t>({ 0, 1, 2 }));
    REQUIRE(mesh.primitives.size() == 1);
    CHECK(mesh.primitives[0].indexCount == 3);

    REQUIRE(loaded.materials.size() == 1);
    CHECK(loaded.materials[0].materialPath == "materials/stone.kmat");
    REQUIRE(loaded.nodes.size() == 1);
    CHECK_NEAR(loaded.nodes[0].translation[1], 3.0, 1e-6);
    REQUIRE(loaded.bones.size() == 1);
    CHECK(loaded.bones[0].name == "spine");

    REQUIRE(loaded.sockets.size() == 1);
    CHECK(loaded.sockets[0].name == "hand_r");
    CHECK(loaded.sockets[0].boneIndex == 0);
    CHECK_NEAR(loaded.sockets[0].translation[2], 0.25, 1e-6);
    REQUIRE(loaded.colliders.size() == 1);
    CHECK(loaded.colliders[0].shape == KModelColliderShape::Capsule);
    CHECK_NEAR(loaded.colliders[0].halfHeight, 0.6, 1e-6);

    CHECK(loaded.defaultMaterial == "materials/skin.mat");
    REQUIRE(loaded.children.size() == 1);
    CHECK(loaded.children[0].name == "gun");
    CHECK(loaded.children[0].modelPath == "models/pistol.kmodel");
    CHECK(loaded.children[0].materialPath == "materials/gun.mat");
    CHECK(loaded.children[0].animationPath == "anims/gun_idle.kanim");
    CHECK(loaded.children[0].boneIndex == 0);
    CHECK_NEAR(loaded.children[0].translation[0], 0.1, 1e-6);
    CHECK_NEAR(loaded.children[0].scale[2], 2.0, 1e-6);
}

TEST_CASE("kmodel: sockets and colliders on missing bones are rejected")
{
    kestest::TempDir dir;
    const std::string path = dir.File("bad.kmodel");
    KModelData model = MakeModel();
    model.sockets[0].boneIndex = 4;
    REQUIRE(SaveKModelFile(path.c_str(), model));
    KModelData loaded;
    CHECK(!LoadKModelFile(path.c_str(), loaded));

    model = MakeModel();
    model.colliders[0].shape = (KModelColliderShape)7;
    REQUIRE(SaveKModelFile(path.c_str(), model));
    CHECK(!LoadKModelFile(path.c_str(), loaded));

    model = MakeModel();
    model.children[0].boneIndex = 3;
    REQUIRE(SaveKModelFile(path.c_str(), model));
    CHECK(!LoadKModelFile(path.c_str(), loaded));
}

TEST_CASE("kmodel: re-import keeps sockets, colliders, children and the default material")
{
    kestest::TempDir dir;
    const std::string path = dir.File("model.kmodel");
    REQUIRE(SaveKModelFile(path.c_str(), MakeModel()));

    KModelData reimported = MakeModel();
    reimported.sockets.clear();
    reimported.colliders.clear();
    reimported.children.clear();
    reimported.defaultMaterial.clear();
    KModelBone extra;
    extra.name = "pelvis";
    extra.nodeIndex = 0;
    reimported.bones.insert(reimported.bones.begin(), extra);

    CHECK(CarryOverKModelAttachments(path.c_str(), reimported) == 3);
    REQUIRE(reimported.sockets.size() == 1);
    CHECK(reimported.sockets[0].boneIndex == 1);
    REQUIRE(reimported.colliders.size() == 1);
    CHECK(reimported.colliders[0].boneIndex == 1);
    REQUIRE(reimported.children.size() == 1);
    CHECK(reimported.children[0].boneIndex == 1);
    CHECK(reimported.defaultMaterial == "materials/skin.mat");
    CHECK(CarryOverKModelAttachments(dir.File("missing.kmodel").c_str(), reimported) == 0);
}

namespace
{
    KModelMesh MakeQuadStrip(const char* name, uint32_t triangles)
    {
        KModelMesh mesh;
        mesh.name = name;
        mesh.vertices.resize(3);
        mesh.vertices[1].px = 1.0f;
        mesh.vertices[2].py = 1.0f;
        for (uint32_t i = 0; i < triangles; ++i)
            mesh.indices.insert(mesh.indices.end(), { 0, 1, 2 });
        return mesh;
    }
}

TEST_CASE("kmodel: primitives become mesh sections with their material slots")
{
    KModelData model;
    model.materials.push_back({ "body", "materials/brick.mat" });
    model.materials.push_back({ "glass", "" });
    KModelMesh first = MakeQuadStrip("hull", 4);
    first.primitives.push_back({ 0, 3, 0 });
    first.primitives.push_back({ 3, 3, 0 }); // same slot right after: merged
    first.primitives.push_back({ 9, 3, 1 }); // indices 6..8 belong to no primitive
    model.meshes.push_back(first);
    model.meshes.push_back(MakeQuadStrip("window", 1)); // no primitives at all
    model.nodes.resize(2);
    model.nodes[0].meshIndex = 0;
    model.nodes[1].meshIndex = 1;

    MeshData mesh;
    REQUIRE(BuildMeshDataFromKModel(model, mesh));
    struct Expected { uint32_t first, count; int32_t slot; };
    const Expected expected[] = { { 0, 6, 0 }, { 6, 3, -1 }, { 9, 3, 1 }, { 12, 3, -1 } };
    REQUIRE(mesh.sections.size() == 4);
    for (size_t i = 0; i < 4; ++i)
    {
        CHECK(mesh.sections[i].firstIndex == expected[i].first);
        CHECK(mesh.sections[i].indexCount == expected[i].count);
        CHECK(mesh.sections[i].materialSlot == expected[i].slot);
    }
}

TEST_CASE("kmodel: re-import keeps slot materials and slots picked for parts without one")
{
    kestest::TempDir dir;
    const std::string path = dir.File("model.kmodel");

    KModelData existing;
    existing.materials.push_back({ "body", "materials/brick.mat" });
    existing.materials.push_back({ "trim", "materials/trim.mat" });
    KModelMesh hull = MakeQuadStrip("hull", 2);
    hull.primitives.push_back({ 0, 3, 0 });
    hull.primitives.push_back({ 3, 3, 1 }); // picked in the model editor
    existing.meshes.push_back(hull);
    existing.nodes.resize(1);
    existing.nodes[0].meshIndex = 0;
    REQUIRE(SaveKModelFile(path.c_str(), existing));

    KModelData reimported;
    reimported.materials.push_back({ "body", "" });
    KModelMesh newHull = MakeQuadStrip("hull", 2);
    newHull.primitives.push_back({ 0, 3, 0 });
    newHull.primitives.push_back({ 3, 3, -1 }); // the source has no material here
    reimported.meshes.push_back(newHull);
    reimported.nodes = existing.nodes;

    CarryOverKModelAttachments(path.c_str(), reimported);
    REQUIRE(reimported.materials.size() == 2);
    CHECK(reimported.materials[0].materialPath == "materials/brick.mat");
    CHECK(reimported.materials[1].name == "trim");
    CHECK(reimported.materials[1].materialPath == "materials/trim.mat");
    CHECK(reimported.meshes[0].primitives[0].materialIndex == 0);
    CHECK(reimported.meshes[0].primitives[1].materialIndex == 1);
    CHECK(FindKModelMaterialSlot(reimported, "trim") == 1);
    CHECK(FindKModelMaterialSlot(reimported, "missing") == -1);

    MeshData flat;
    REQUIRE(BuildMeshDataFromKModel(reimported, flat));
    KModelMesh converted;
    ConvertMeshDataToKModelMesh(flat, converted);
    REQUIRE(converted.primitives.size() == 2);
    CHECK(converted.primitives[1].firstIndex == 3);
    CHECK(converted.primitives[1].materialIndex == 1);
}

TEST_CASE("kmodel: rejects truncated and foreign files")
{
    kestest::TempDir dir;
    const std::string path = dir.File("model.kmodel");
    REQUIRE(SaveKModelFile(path.c_str(), MakeModel()));
    CheckRejectsCorruption(dir, path, LoadModel);
}

TEST_CASE("ksound: round trip keeps the samples")
{
    kestest::TempDir dir;
    const std::string path = dir.File("tone.ksound");
    const SoundFileData sound = MakeSound();
    REQUIRE(SaveKSoundFile(path.c_str(), sound));

    SoundFileData loaded;
    REQUIRE(LoadKSoundFile(path.c_str(), loaded));
    CHECK(loaded.sourcePath == sound.sourcePath);
    CHECK(loaded.sampleRate == sound.sampleRate);
    CHECK(loaded.channels == sound.channels);
    CHECK(loaded.frameCount == sound.frameCount);
    CHECK(loaded.samples == sound.samples);
}

TEST_CASE("ksound: rejects truncated and foreign files")
{
    kestest::TempDir dir;
    const std::string path = dir.File("tone.ksound");
    REQUIRE(SaveKSoundFile(path.c_str(), MakeSound()));
    CheckRejectsCorruption(dir, path, LoadSound);
}

TEST_CASE("ktex: round trip keeps the pixels")
{
    kestest::TempDir dir;
    const std::string path = dir.File("checker.ktex");
    const TextureFileData texture = MakeTexture();
    REQUIRE(SaveKTexFile(path.c_str(), texture));

    TextureFileData loaded;
    REQUIRE(LoadKTexFile(path.c_str(), loaded));
    CHECK(loaded.width == texture.width);
    CHECK(loaded.height == texture.height);
    CHECK(loaded.flags == texture.flags);
    CHECK(loaded.pixels == texture.pixels);
}

TEST_CASE("ktex: rejects truncated and foreign files")
{
    kestest::TempDir dir;
    const std::string path = dir.File("checker.ktex");
    REQUIRE(SaveKTexFile(path.c_str(), MakeTexture()));
    CheckRejectsCorruption(dir, path, LoadTexture);
}

namespace
{
    // Smooth gradient with a soft alpha ramp: BC formats should land close to it.
    std::vector<uint8_t> MakeGradient(uint32_t width, uint32_t height)
    {
        std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                uint8_t* p = &rgba[(static_cast<size_t>(y) * width + x) * 4];
                p[0] = static_cast<uint8_t>(x * 255 / (width - 1));
                p[1] = static_cast<uint8_t>(y * 255 / (height - 1));
                p[2] = 128;
                p[3] = static_cast<uint8_t>(255 - x * 128 / (width - 1));
            }
        }
        return rgba;
    }

    int MaxChannelError(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int channels)
    {
        int worst = 0;
        for (size_t i = 0; i < a.size() && i < b.size(); i += 4)
        {
            for (int c = 0; c < channels; ++c)
                worst = std::max(worst, std::abs(int(a[i + c]) - int(b[i + c])));
        }
        return worst;
    }

    TextureFileData CookWith(TextureCompression compression, uint32_t width = 64, uint32_t height = 32)
    {
        TextureGroupSettings settings;
        settings.compression = compression;
        settings.srgb = compression == TextureCompression::Default;
        const std::vector<uint8_t> rgba = MakeGradient(width, height);
        TextureFileData texture;
        CookTexture(rgba.data(), width, height, settings, texture);
        return texture;
    }
}

TEST_CASE("ktex: settings text round trips only the overridden keys")
{
    TextureSettings settings;
    settings.group = "WorldNormalMap";
    settings.overrides = TextureSetting_MaxSize | TextureSetting_AddressU;
    settings.values.maxSize = 512;
    settings.values.addressU = TextureAddress::Mirror;
    settings.values.lodBias = 3; // not overridden, so not written
    const std::string text = SerializeTextureSettings(settings);
    CHECK(text.find("LODBias") == std::string::npos);

    const TextureSettings parsed = ParseTextureSettings(text);
    CHECK(parsed.group == "WorldNormalMap");
    CHECK(parsed.overrides == settings.overrides);
    CHECK(parsed.values.maxSize == 512);
    CHECK(parsed.values.addressU == TextureAddress::Mirror);
}

TEST_CASE("ktex: groups resolve, project ini overrides and adds groups")
{
    std::vector<TextureGroup> groups = DefaultTextureGroups();
    ParseTextureGroups("[Group.World]\nMaxSize=1024\n\n[Group.Terrain]\nCompression=DefaultSmall\nSRGB=True\nLODBias=1\n", groups);
    REQUIRE(FindTextureGroup(groups, "terrain") != nullptr);

    TextureSettings world;
    CHECK(ResolveTextureSettings(world, groups).maxSize == 1024);

    TextureSettings terrain;
    terrain.group = "Terrain";
    const TextureGroupSettings t = ResolveTextureSettings(terrain, groups);
    CHECK(t.compression == TextureCompression::DefaultSmall);
    CHECK(t.lodBias == 1);

    // A normal map is never sRGB, whatever the override says.
    TextureSettings normal;
    normal.group = "WorldNormalMap";
    normal.overrides = TextureSetting_Srgb;
    normal.values.srgb = true;
    CHECK(!ResolveTextureSettings(normal, groups).srgb);

    TextureSettings unknown;
    unknown.group = "Missing";
    CHECK(ResolveTextureSettings(unknown, groups).compression == TextureCompression::Default);
}

TEST_CASE("ktex: import picks a group from the file name")
{
    CHECK(GuessTextureGroup("textures/Brick_N.png") == "WorldNormalMap");
    CHECK(GuessTextureGroup("C:\\art\\brick_normal.tga") == "WorldNormalMap");
    CHECK(GuessTextureGroup("brick_ORM.png") == "WorldMasks");
    CHECK(GuessTextureGroup("brick_roughness.png") == "WorldMasks");
    CHECK(GuessTextureGroup("ui/button.png") == "UI");
    CHECK(GuessTextureGroup("brick.png") == "World");
    CHECK(GuessTextureGroup("_n.png") == "World");
}

TEST_CASE("ktex: cooking builds the mip chain in the group's format")
{
    const TextureFileData bc7 = CookWith(TextureCompression::Default);
    CHECK(bc7.format == TexturePixelFormat::BC7);
    CHECK(bc7.mipCount == 7); // 64x32 .. 1x1
    CHECK((bc7.flags & TextureFileFlag_Srgb) != 0);
    CHECK(bc7.pixels.size() == TextureMipOffset(bc7, bc7.mipCount));
    // 1x1 and 2x2 mips still take a whole block
    CHECK(TextureMipByteSize(TexturePixelFormat::BC7, 1, 1) == 16);
    CHECK(TextureMipByteSize(TexturePixelFormat::BC1, 6, 5) == 2 * 2 * 8);

    CHECK(CookWith(TextureCompression::DefaultSmall).format == TexturePixelFormat::BC1);
    CHECK(CookWith(TextureCompression::Grayscale).format == TexturePixelFormat::BC4);
    const TextureFileData normal = CookWith(TextureCompression::NormalMap);
    CHECK(normal.format == TexturePixelFormat::BC5);
    CHECK((normal.flags & TextureFileFlag_NormalMap) != 0);
    CHECK(CookWith(TextureCompression::UserInterface).format == TexturePixelFormat::RGBA8);
}

TEST_CASE("ktex: BC formats decode close to the source")
{
    const std::vector<uint8_t> source = MakeGradient(64, 32);
    std::vector<uint8_t> decoded;

    REQUIRE(DecodeTextureMip(CookWith(TextureCompression::Default), 0, decoded));
    CHECK(decoded.size() == source.size());
    CHECK(MaxChannelError(source, decoded, 4) <= 12);

    REQUIRE(DecodeTextureMip(CookWith(TextureCompression::DefaultSmall), 0, decoded));
    CHECK(MaxChannelError(source, decoded, 3) <= 24);

    REQUIRE(DecodeTextureMipRaw(CookWith(TextureCompression::Grayscale), 0, decoded));
    CHECK(MaxChannelError(source, decoded, 1) <= 8);
    CHECK(decoded[1] == 0);

    REQUIRE(DecodeTextureMipRaw(CookWith(TextureCompression::NormalMap), 0, decoded));
    CHECK(MaxChannelError(source, decoded, 2) <= 8);

    // Odd sizes: the last partial blocks are cropped.
    REQUIRE(DecodeTextureMip(CookWith(TextureCompression::Default, 13, 7), 0, decoded));
    CHECK(decoded.size() == 13u * 7u * 4u);
}

TEST_CASE("ktex: max size and LOD bias drop the top mips")
{
    TextureGroupSettings settings;
    settings.compression = TextureCompression::Uncompressed;
    settings.maxSize = 32;
    settings.lodBias = 1;
    const std::vector<uint8_t> rgba = MakeGradient(128, 64);
    TextureFileData texture;
    REQUIRE(CookTexture(rgba.data(), 128, 64, settings, texture));
    CHECK(texture.width == 16);
    CHECK(texture.height == 8);
    CHECK(texture.mipCount == 5);

    settings.mipGen = TextureMipGen::NoMipmaps;
    REQUIRE(CookTexture(rgba.data(), 128, 64, settings, texture));
    CHECK(texture.mipCount == 1);
}

TEST_CASE("ktex: v2 keeps settings and source, packaging strips the source")
{
    TextureFileData texture = CookWith(TextureCompression::NormalMap);
    texture.sourcePath = "brick_n.png";
    texture.settings.group = "WorldNormalMap";
    texture.settings.overrides = TextureSetting_MaxSize;
    texture.settings.values.maxSize = 256;
    texture.sourceData = { 0x89, 'P', 'N', 'G', 1, 2, 3 };

    std::vector<uint8_t> bytes;
    REQUIRE(WriteKTex(texture, bytes, nullptr));
    TextureFileData loaded;
    REQUIRE(ReadKTex(bytes.data(), bytes.size(), loaded, nullptr));
    CHECK(loaded.version == 2);
    CHECK(loaded.format == TexturePixelFormat::BC5);
    CHECK(loaded.mipCount == texture.mipCount);
    CHECK(loaded.pixels == texture.pixels);
    CHECK(loaded.settings.group == "WorldNormalMap");
    CHECK(loaded.settings.values.maxSize == 256);
    CHECK(loaded.sourceData == texture.sourceData);

    REQUIRE(ReadKTex(bytes.data(), bytes.size(), loaded, nullptr, false));
    CHECK(loaded.sourceData.empty());

    std::vector<uint8_t> stripped;
    REQUIRE(WriteKTex(texture, stripped, nullptr, true));
    CHECK(stripped.size() == bytes.size() - texture.sourceData.size());
    REQUIRE(ReadKTex(stripped.data(), stripped.size(), loaded, nullptr));
    CHECK(loaded.sourceData.empty());
    CHECK(loaded.pixels == texture.pixels);
}
