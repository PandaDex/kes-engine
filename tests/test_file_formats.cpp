#include "test_framework.h"
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
