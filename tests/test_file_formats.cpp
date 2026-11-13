#include "test_framework.h"
#include <cstring>
#include <filesystem>
#include <sstream>
#include "animation_clip.h"
#include "kmodel.h"
#include "sound_file.h"
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
}

TEST_CASE("kmodel: re-import keeps sockets and colliders by bone name")
{
    kestest::TempDir dir;
    const std::string path = dir.File("model.kmodel");
    REQUIRE(SaveKModelFile(path.c_str(), MakeModel()));

    KModelData reimported = MakeModel();
    reimported.sockets.clear();
    reimported.colliders.clear();
    KModelBone extra;
    extra.name = "pelvis";
    extra.nodeIndex = 0;
    reimported.bones.insert(reimported.bones.begin(), extra);

    CHECK(CarryOverKModelAttachments(path.c_str(), reimported) == 2);
    REQUIRE(reimported.sockets.size() == 1);
    CHECK(reimported.sockets[0].boneIndex == 1);
    REQUIRE(reimported.colliders.size() == 1);
    CHECK(reimported.colliders[0].boneIndex == 1);
    CHECK(CarryOverKModelAttachments(dir.File("missing.kmodel").c_str(), reimported) == 0);
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
