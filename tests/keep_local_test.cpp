#include "test_client.hpp"
#include "fake_s3.hpp"
#include "media/keep_local.hpp"
#include "upload/media_fetch.hpp"

#include <doctest.h>

#include <atomic>
#include <thread>

namespace {

const std::string kA(64, 'a');
const std::string kB(64, 'b');
const std::string kC(64, 'c');

bool waitFor(const std::function<bool()>& done, double seconds = 10.0)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

} // namespace

TEST_SUITE_BEGIN("keeplocal");

TEST_CASE("a clip's own choice wins over its shows; the choices persist and are read again when changed on disk")
{
    TempDir dir;
    const fs::path file = dir.path / "keep-local.json";
    {
        azl::KeepLocal keep(file);
        keep.setShow("venue:/shows/a.azl", true, {kA, kB});
        CHECK(keep.chosen(kA));
        CHECK(keep.chosen(kB));
        CHECK_FALSE(keep.chosen(kC));

        keep.setClip(kB, false);
        keep.setClip(kC, true);
        CHECK(keep.chosen() == std::set<std::string>{kA, kC});
        CHECK(keep.clip(kB) == false);
        CHECK_FALSE(keep.clip(kA));

        CHECK(keep.updateShow("venue:/shows/a.azl", {kB}));
        CHECK_FALSE(keep.updateShow("venue:/shows/other.azl", {kA}));
        CHECK(keep.chosen() == std::set<std::string>{kC});
    }

    azl::KeepLocal again(file);
    CHECK(again.show("venue:/shows/a.azl"));
    CHECK(again.chosen() == std::set<std::string>{kC});
    CHECK_FALSE(again.reload());

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    azl::KeepLocal other(file);
    other.setClip(kB, std::nullopt);
    other.setShow("venue:/shows/a.azl", false);
    CHECK(again.reload());
    CHECK_FALSE(again.show("venue:/shows/a.azl"));
    CHECK(again.chosen() == std::set<std::string>{kC});
}

TEST_CASE("the still cache keeps one JPEG per media hash and takes only hashes")
{
    TempDir dir;
    azl::StillCache cache(dir.path / "stills");
    CHECK_FALSE(cache.find(kA));
    cache.store(kA, {0xFF, 0xD8, 1});
    cache.store("../escape", {1});
    CHECK(cache.find(kA) == std::vector<uint8_t>{0xFF, 0xD8, 1});
    CHECK(azl::StillCache(dir.path / "stills").find(kA));
    CHECK_FALSE(cache.find("../escape"));
    CHECK_FALSE(fs::exists(dir.path / "escape.jpg"));
}

TEST_CASE("a fetch by hash comes down in chunks, resumes after a drop and rejects other content")
{
    FakeS3 s3;
    TempDir dir;
    azl::S3Client client(s3.config());
    const std::string data = bytes(100000);
    const std::string hash = azl::sha256Hex(data);
    s3.putObject(hash, data);
    const fs::path file = azl::fetchedFile(dir.path, hash);

    SUBCASE("in chunks")
    {
        azl::fetchObject(client, hash, file, 30000);
        CHECK(readAll(file) == data);
        CHECK(s3.ranges().size() == 4);
        azl::fetchObject(client, hash, file, 30000);
        CHECK(s3.ranges().size() == 4);
    }
    SUBCASE("a dropped connection goes on where it stopped")
    {
        s3.dropAfterGets(2);
        CHECK_THROWS(azl::fetchObject(client, hash, file, 30000));
        CHECK(fs::file_size(file.string() + ".part") == 60000);
        s3.restore();
        azl::fetchObject(client, hash, file, 30000);
        CHECK(readAll(file) == data);
        CHECK(s3.ranges().back() == "bytes=90000-99999");
        CHECK(s3.ranges().size() == 4);
    }
    SUBCASE("other content is deleted")
    {
        s3.corrupt(hash, 1);
        CHECK_THROWS_WITH(azl::fetchObject(client, hash, file, 30000), doctest::Contains("other content"));
        CHECK_FALSE(fs::exists(file));
        CHECK_FALSE(fs::exists(file.string() + ".part"));
    }
    SUBCASE("an object not in the bucket")
    {
        CHECK_THROWS_WITH(azl::fetchObject(client, kA, azl::fetchedFile(dir.path, kA)),
                          doctest::Contains("not in the bucket"));
    }
}

TEST_CASE("a fetched file is named only by a content hash")
{
    TempDir dir;
    CHECK(azl::fetchedFile(dir.path, kA) == dir.path / (std::string(kA) + ".mp4"));
    CHECK_THROWS(azl::fetchedFile(dir.path, "../escape"));
    CHECK_THROWS(azl::fetchedFile(dir.path, "/etc/passwd"));
    CHECK_THROWS(azl::fetchedFile(dir.path, std::string(kA).substr(0, 63) + "/"));
}

TEST_CASE("the fetcher brings wanted media one at a time, keeps a failure until wanted again")
{
    FakeS3 s3;
    TempDir dir;
    const std::string data = bytes(5000);
    const std::string hash = azl::sha256Hex(data);
    s3.putObject(hash, data);
    std::mutex mutex;
    std::vector<std::string> arrived;
    azl::MediaFetcher fetcher([&] { return std::make_shared<azl::S3Client>(s3.config()); }, dir.path,
                              [&](const std::string& h, const fs::path& file) {
                                  std::lock_guard lock(mutex);
                                  arrived.push_back(h);
                                  CHECK(readAll(file) == data);
                              });
    auto count = [&] {
        std::lock_guard lock(mutex);
        return arrived.size();
    };

    fetcher.want(hash);
    REQUIRE(waitFor([&] { return fetcher.state(hash) == std::nullopt && count() == 1; }));

    fetcher.want(kA);
    REQUIRE(waitFor([&] { return fetcher.state(kA) == azl::MediaFetcher::State::Failed; }));
    CHECK(fetcher.error(kA).find("not in the bucket") != std::string::npos);
    s3.putObject(kA, data);
    fetcher.want(kA);
    REQUIRE(waitFor([&] { return fetcher.state(kA) == azl::MediaFetcher::State::Failed && s3.count("head") >= 3; }));
    CHECK(fetcher.error(kA).find("other content") != std::string::npos);
    CHECK(count() == 1);
}

TEST_CASE("a still request is rendered once on the engine and served from its cache after")
{
    azl::EngineHarness harness({.width = 320, .height = 180}, 30.0);
    TempDir media;
    Client client(harness.document());
    const Message added = addMedia(client, copyMedia("count_240.mp4", media.path / "count_240.mp4"));
    const std::string hash = added["entry"]["hash"];

    for (int i = 0; i < 2; ++i) {
        client.send({{"type", "library"}, {"op", "still"}, {"hash", hash}});
        const Message still = client.next();
        CHECK(still["op"] == "still");
        CHECK(still["hash"] == hash);
        REQUIRE(still.contains("jpeg"));
    }
    CHECK(harness.stills().asked.load() == 2);
    CHECK(harness.stills().rendered.load() == 1);

    client.send({{"type", "library"}, {"op", "still"}, {"hash", kA}});
    CHECK(client.next().contains("error"));
}

TEST_SUITE_END();
