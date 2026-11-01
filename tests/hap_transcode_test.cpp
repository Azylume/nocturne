#include "hap_clips.hpp"
#include "test_paths.hpp"

#include "media/hap_encoder.hpp"
#include "media/media_library.hpp"
#include "upload/media_upload.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#endif

#include <fstream>

namespace {

fs::path hapEncodeTool()
{
    const char* path = std::getenv("NOCTURNE_HAP_ENCODE");
    return path ? path : NOCTURNE_HAP_ENCODE;
}

azl::NativeHandle openForReading(const fs::path& file)
{
#ifdef _WIN32
    return CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
#else
    return ::open(file.c_str(), O_RDONLY);
#endif
}

// Runs the tool with stdin from `input`; returns its exit status.
int runWithInput(const std::vector<std::string>& args, const fs::path& input)
{
    azl::SpawnOptions options{.program = args[0], .args = {args.begin() + 1, args.end()}};
    options.input = azl::Stdio::to(openForReading(input));
    options.error = azl::Stdio::null();
    std::string error;
    std::optional<azl::Process> tool = azl::spawnProcess(options, &error);
    azl::closeHandle(options.input.handle);
    REQUIRE_MESSAGE(tool, error);
    return tool->wait();
}

} // namespace

TEST_CASE("the HAP encoder takes raw frames in pieces of any size and writes one movie frame per picture")
{
    TempDir dir;
    const fs::path file = dir.path / "count.mov";
    azl::HapEncoder encoder(file, azl::HapCodec::HapAlpha, kHapWidth, kHapHeight, 30000, 1001, 2);
    for (int i = 0; i < 5; ++i) {
        const std::vector<uint8_t> picture = countPicture(i, true);
        for (size_t at = 0; at < picture.size(); at += 9999)
            encoder.feed(picture.data() + at, std::min<size_t>(9999, picture.size() - at));
    }
    encoder.finish();
    CHECK(encoder.frames() == 5);
    CHECK(azl::hapCodecOfFile(file) == azl::HapCodec::HapAlpha);
    CHECK(azl::readHapFrames(file, 10).size() == 5);
    CHECK(azl::probeMedia(file).status == azl::MediaStatus::Ok);

    azl::HapEncoder partial(dir.path / "partial.mov", azl::HapCodec::Hap, kHapWidth, kHapHeight, 30, 1);
    const std::vector<uint8_t> picture = countPicture(0, false);
    partial.feed(picture.data(), picture.size() - 1);
    CHECK_THROWS(partial.finish());
    CHECK(azl::hapCodecNamed("hap-q-alpha") == azl::HapCodec::HapQAlpha);
    CHECK(azl::hapCodecNamed("dxv") == azl::HapCodec::None);
}

TEST_CASE("the HAP encode tool turns raw RGBA on stdin into a HAP movie the engine plays, also named .mp4")
{
    const fs::path tool = hapEncodeTool();
    if (!fs::exists(tool)) {
        MESSAGE("skipped: no " << tool);
        return;
    }
    TempDir dir;
    const fs::path raw = dir.path / "frames.rgba";
    {
        std::ofstream out(raw, std::ios::binary);
        for (int i = 0; i < 6; ++i) {
            const std::vector<uint8_t> picture = countPicture(i, false);
            out.write(reinterpret_cast<const char*>(picture.data()), std::streamsize(picture.size()));
        }
    }
    const fs::path file = dir.path / "count.mov";
    CHECK(runWithInput({tool.string(), "--size", "320x180", "--fps", "30", "--codec", "hap-q", "--chunks", "3",
                        file.string()},
                       raw) == 0);
    CHECK(azl::hapCodecOfFile(file) == azl::HapCodec::HapQ);
    CHECK(azl::readHapFrames(file, 10).size() == 6);
    const fs::path renamed = dir.path / "count.mp4";
    fs::copy_file(file, renamed);
    CHECK(azl::probeMedia(renamed).status == azl::MediaStatus::Ok);

    CHECK(runWithInput({tool.string(), "--size", "320x181", "--fps", "30", (dir.path / "bad.mov").string()}, raw) != 0);
    CHECK(runWithInput({tool.string(), "--size", "320x180", "--fps", "30", "--codec", "dxv",
                        (dir.path / "bad.mov").string()},
                       raw) == 2);
}

TEST_CASE("conversion to HAP gives a HAP movie with every frame of the source, cached by source and codec")
{
    if (ffmpegMissing())
        return;
    TempDir dir;
    const fs::path source = mediaDir() / "count_709_tv.mp4";
    azl::ConvertOptions options;
    options.codec = "hap-q";
    const azl::Conversion conversion = azl::convertForPlayback(source, dir.path, options);
    CHECK(conversion.kind == "hap");
    CHECK(conversion.file.extension() == ".mov");
    CHECK(azl::hapCodecOfFile(conversion.file) == azl::HapCodec::HapQ);
    CHECK(azl::readHapFrames(conversion.file, 100).size() == 60);
    CHECK(azl::convertForPlayback(source, dir.path, options).kind == "cached");
    CHECK(azl::convertForPlayback(source, dir.path).kind != "cached");

    azl::EngineHarness harness({.width = 960, .height = 540}, 30.0);
    Client client(harness.document());
    playMedia(client, conversion.file, {azl::SyncMode::Timeline, azl::PlayMode::Loop, 0});
    CHECK(frameAfterStep(harness, 11) == 10);
    // ffmpeg's Y'CbCr to RGB and HAP Q's own rounding: within 10 of the source colour.
    const azl::Image screen = harness.readScreen();
    const uint8_t* patch = pixel(screen, 240, 405);
    CHECK(std::abs(patch[0] - 200) <= 10);
    CHECK(std::abs(patch[1] - 40) <= 10);
    CHECK(std::abs(patch[2] - 40) <= 10);

    options.codec = "dxv";
    CHECK_THROWS(azl::convertForPlayback(source, dir.path / "x", options));
}
