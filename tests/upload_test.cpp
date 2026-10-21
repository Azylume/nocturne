#include "test_client.hpp"
#include "test_paths.hpp"
#include "fake_s3.hpp"
#include "media/media_library.hpp"
#include "upload/media_upload.hpp"

#include <doctest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace {

std::string shell(const std::string& command)
{
    std::string out;
    FILE* pipe = popen(command.c_str(), "r");
    REQUIRE(pipe);
    char buffer[4096];
    for (size_t n; (n = fread(buffer, 1, sizeof buffer, pipe)) > 0;)
        out.append(buffer, n);
    pclose(pipe);
    return out;
}

struct CtlRun {
    std::string out;
    int status = 0;
};

// The ctl binary with `home` as HOME and the fake bucket's settings in the environment.
CtlRun ctlAt(const fs::path& home, const FakeS3& s3, const std::string& args)
{
    const fs::path binary = ctlPath();
    const std::string command = "HOME='" + home.string() + "' NOCTURNE_R2_ENDPOINT=" + s3.endpoint() +
                                " NOCTURNE_R2_BUCKET=" + FakeS3::kBucket + " NOCTURNE_R2_ACCESS_KEY_ID=" +
                                FakeS3::kKeyId + " NOCTURNE_R2_SECRET_ACCESS_KEY=" + FakeS3::kSecret + " '" +
                                binary.string() + "' " + args + " 2>&1";
    CtlRun run;
    FILE* pipe = popen(command.c_str(), "r");
    REQUIRE(pipe);
    char buffer[512];
    while (fgets(buffer, sizeof buffer, pipe))
        run.out += buffer;
    const int wait = pclose(pipe);
    run.status = WEXITSTATUS(wait);
    return run;
}

std::vector<double> keyframeTimes(const fs::path& file)
{
    std::vector<double> times;
    std::istringstream in(shell("ffprobe -v error -select_streams v:0 -show_entries packet=pts_time,flags -of csv=p=0 '" +
                                file.string() + "'"));
    for (std::string line; std::getline(in, line);)
        if (line.find(",K") != std::string::npos)
            times.push_back(std::stod(line));
    std::sort(times.begin(), times.end());
    return times;
}

} // namespace

TEST_SUITE_BEGIN("upload");

TEST_CASE("SigV4 signs the AWS documentation's GET Object example")
{
    const std::string empty = azl::sha256Hex("");
    CHECK(empty == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string auth = azl::signV4(
        "GET", "/test.txt", "",
        {{"host", "examplebucket.s3.amazonaws.com"}, {"range", "bytes=0-9"}, {"x-amz-content-sha256", empty},
         {"x-amz-date", "20130524T000000Z"}},
        empty, "20130524T000000Z", "us-east-1", "AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY");
    CHECK(auth == "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
                  "SignedHeaders=host;range;x-amz-content-sha256;x-amz-date, "
                  "Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
}

TEST_CASE("a file goes up under its hash once; an object already there is not uploaded again")
{
    TempDir dir;
    FakeS3 s3;
    azl::S3Client client(s3.config());
    const fs::path file = dir.path / "clip.mp4";
    writeAll(file, bytes(3000));
    const std::string hash = azl::sha256File(file);
    const azl::UploadOptions options{.partSize = 1 << 20, .stateDir = dir.path / "pending", .attempts = 1};

    CHECK(azl::uploadObject(client, file, hash, options).state == "uploaded");
    CHECK(s3.objects().at(hash) == readAll(file));
    CHECK(azl::uploadObject(client, file, hash, options).state == "present");
    CHECK(s3.count("put") == 1);
}

TEST_CASE("a multipart upload cut off midway resumes with the parts already uploaded")
{
    TempDir dir;
    FakeS3 s3;
    azl::S3Client client(s3.config());
    const fs::path file = dir.path / "clip.mp4";
    writeAll(file, bytes(4500));
    const azl::UploadOptions options{.partSize = 1000, .stateDir = dir.path / "pending", .attempts = 1};

    s3.dropAfterParts(2);
    CHECK_THROWS_AS(azl::uploadObject(client, file, "k", options), azl::TransportError);
    CHECK(s3.count("part") == 2);
    CHECK(fs::exists(dir.path / "pending/k.json"));
    CHECK(s3.objects().empty());

    s3.restore();
    const azl::UploadResult result = azl::uploadObject(client, file, "k", options);
    CHECK(result.state == "uploaded");
    CHECK(result.parts == 5);
    CHECK(result.resumedParts == 2);
    CHECK(s3.count("create") == 1);
    CHECK(s3.count("part") == 5);
    CHECK(s3.objects().at("k") == readAll(file));
    CHECK_FALSE(fs::exists(dir.path / "pending/k.json"));
}

TEST_CASE("an upload whose multipart state is gone on the server starts over")
{
    TempDir dir;
    FakeS3 s3;
    azl::S3Client client(s3.config());
    const fs::path file = dir.path / "clip.mp4";
    writeAll(file, bytes(2500));
    const azl::UploadOptions options{.partSize = 1000, .stateDir = dir.path / "pending", .attempts = 1};
    fs::create_directories(options.stateDir);
    writeAll(options.stateDir / "k.json",
             nlohmann::json{{"for",
                             {{"endpoint", s3.endpoint()}, {"bucket", "media"}, {"size", 2500}, {"part_size", 1000}}},
                            {"upload_id", "expired"}}
                 .dump());

    const azl::UploadResult result = azl::uploadObject(client, file, "k", options);
    CHECK(result.resumedParts == 0);
    CHECK(s3.count("create") == 1);
    CHECK(s3.objects().at("k") == readAll(file));
}

TEST_CASE("wrong upload credentials fail with a message that says so")
{
    TempDir dir;
    FakeS3 s3;
    const fs::path file = dir.path / "clip.mp4";
    writeAll(file, bytes(100));
    const azl::UploadOptions options{.stateDir = dir.path / "pending", .attempts = 1};

    azl::S3Client wrongSecret(s3.config("not-the-secret"));
    try {
        azl::uploadObject(wrongSecret, file, "k", options);
        FAIL("no error");
    } catch (const azl::S3Error& e) {
        CHECK(e.status == 403);
    }

    if (ffmpegMissing())
        return;
    azl::S3Client wrongKey(s3.config(FakeS3::kSecret, "someone-else"));
    const azl::RunOptions run{.convertDir = dir.path / "converted",
                              .manifest = dir.path / "run.json",
                              .upload = options,
                              .states = dir.path / "states.json"};
    const std::vector<fs::path> sources{mediaDir() / "count_100.mp4", mediaDir() / "count_240.mp4"};
    CHECK_THROWS_WITH(azl::runUpload(&wrongKey, sources, run),
                      doctest::Contains("R2 refused the credentials for bucket media"));
    const auto manifest = nlohmann::json::parse(readAll(dir.path / "run.json"));
    REQUIRE(manifest["files"].size() == 1);
    CHECK(manifest["files"][0]["status"] == "failed");
    CHECK(s3.objects().empty());
}

TEST_CASE("missing R2 settings or secret fail with what to set")
{
    for (const char* name : {"NOCTURNE_R2_ENDPOINT", "NOCTURNE_R2_BUCKET", "NOCTURNE_R2_ACCESS_KEY_ID",
                             "NOCTURNE_R2_SECRET_ACCESS_KEY"})
        unsetenv(name);
    TempDir dir;
    const fs::path config = dir.path / "r2.json";
    CHECK_THROWS_WITH(azl::loadR2Config(config), doctest::Contains("endpoint, bucket, access_key_id"));

    writeAll(config, R"({"endpoint": "https://example.invalid", "bucket": "media", "access_key_id": "id"})");
    CHECK_THROWS_WITH(azl::loadR2Config(config, "nocturne-r2-test-no-such-item"),
                      doctest::Contains("no R2 secret access key: add it to the login keychain"));

    writeAll(config, R"({"endpoint": "https://example.invalid", "bucket": "media", "access_key_id": "id",
                         "secret_access_key": "s"})");
    fs::permissions(config, fs::perms::owner_read | fs::perms::owner_write | fs::perms::others_read);
    CHECK_THROWS_WITH(azl::loadR2Config(config), doctest::Contains("others can read it"));
    fs::permissions(config, fs::perms::owner_read | fs::perms::owner_write);
    const azl::S3Config loaded = azl::loadR2Config(config);
    CHECK(loaded.endpoint == "https://example.invalid");
    CHECK(loaded.secretAccessKey == "s");
}

TEST_CASE("conversion gives H.264 with a keyframe every second, no audio, and a stable hash")
{
    if (ffmpegMissing())
        return;
    TempDir dir;
    const fs::path source = mediaDir() / "count_240.mp4";
    const azl::Conversion first = azl::convertForPlayback(source, dir.path / "a");
    const azl::Conversion second = azl::convertForPlayback(source, dir.path / "b");
    CHECK(first.kind == "encoded");
    CHECK(azl::sha256File(first.file) == azl::sha256File(second.file));
    CHECK(azl::convertForPlayback(source, dir.path / "a").kind == "cached");

    CHECK(shell("ffprobe -v error -show_entries stream=codec_type,codec_name -of csv=p=0 '" + first.file.string() +
                "'") == "h264,video\n");
    const std::vector<double> keyframes = keyframeTimes(first.file);
    REQUIRE(keyframes.size() == 8);
    for (size_t i = 0; i < keyframes.size(); ++i)
        CHECK(keyframes[i] == doctest::Approx(double(i)).epsilon(0.01));

    // count_100 already has a keyframe every second: copied as it is, not encoded again.
    CHECK(azl::convertForPlayback(mediaDir() / "count_100.mp4", dir.path / "a").kind == "remuxed");
}

TEST_CASE("an upload run writes a manifest with each file's hash and state")
{
    if (ffmpegMissing())
        return;
    TempDir dir;
    FakeS3 s3;
    azl::S3Client client(s3.config());
    const std::vector<fs::path> sources{mediaDir() / "count_100.mp4", mediaDir() / "count_240.mp4",
                                        dir.path / "missing.mp4"};
    azl::RunOptions run{.convertDir = dir.path / "converted",
                        .manifest = dir.path / "first.json",
                        .upload = {.partSize = 6000, .stateDir = dir.path / "pending", .attempts = 1},
                        .states = dir.path / "states.json"};

    const nlohmann::json manifest = azl::runUpload(&client, sources, run);
    CHECK(nlohmann::json::parse(readAll(run.manifest)) == manifest);
    CHECK(manifest["endpoint"] == s3.endpoint());
    CHECK(manifest["bucket"] == "media");
    REQUIRE(manifest["files"].size() == 3);
    for (int i : {0, 1}) {
        const nlohmann::json& file = manifest["files"][i];
        CHECK(file["source"] == sources[size_t(i)].string());
        CHECK(file["status"] == "uploaded");
        CHECK(file["hash"] == azl::sha256File(file["converted"].get<std::string>()));
        CHECK(s3.objects().at(file["hash"]) == readAll(file["converted"].get<std::string>()));
    }
    CHECK(manifest["files"][0]["conversion"] == "remuxed");
    CHECK(manifest["files"][1]["conversion"] == "encoded");
    CHECK(manifest["files"][1]["parts"].get<int>() > 1);
    CHECK(manifest["files"][2]["status"] == "failed");
    CHECK(manifest["files"][2].contains("error"));
    CHECK(manifest["summary"] == nlohmann::json{{"uploaded", 2}, {"failed", 1}});

    run.manifest = dir.path / "second.json";
    const nlohmann::json again = azl::runUpload(&client, {sources[0], sources[1]}, run);
    CHECK(again["summary"] == nlohmann::json{{"present", 2}});
    CHECK(again["files"][0]["conversion"] == "cached");
    CHECK(again["files"][0]["hash"] == manifest["files"][0]["hash"]);
    CHECK(s3.count("put") + s3.count("create") == 2);
}

TEST_CASE("each upload's state is kept by hash: uploading with its bytes, uploaded; a drop leaves it failed")
{
    if (ffmpegMissing())
        return;
    TempDir dir;
    FakeS3 s3;
    azl::S3Client client(s3.config());
    const fs::path source = mediaDir() / "count_240.mp4";
    const fs::path file = dir.path / "states.json";
    std::vector<azl::UploadState> seen;
    azl::RunOptions run{.convertDir = dir.path / "converted",
                        .manifest = dir.path / "run.json",
                        .upload = {.partSize = 6000,
                                   .stateDir = dir.path / "pending",
                                   .attempts = 1,
                                   .progress = [&](uint64_t, uint64_t) { seen.push_back(azl::UploadStates(file).all().begin()->second); }},
                        .states = file};

    s3.dropAfterParts(2);
    const nlohmann::json first = azl::runUpload(&client, {source}, run);
    const std::string hash = first["files"][0]["hash"];
    const uint64_t size = first["files"][0]["size"];
    REQUIRE(seen.size() == 3);
    for (const azl::UploadState& state : seen) {
        CHECK(state.kind == azl::UploadState::Kind::Uploading);
        CHECK(state.size == size);
        CHECK(state.source == source.string());
        CHECK(state.pid == getpid());
    }
    CHECK(seen[0].bytes == 0);
    CHECK(seen[2].bytes == 12000);
    const std::optional<azl::UploadState> failed = azl::UploadStates(file).find(hash);
    REQUIRE(failed);
    CHECK(failed->kind == azl::UploadState::Kind::Failed);
    CHECK(failed->bytes == 12000);
    CHECK_FALSE(failed->error.empty());

    s3.restore();
    seen.clear();
    azl::runUpload(&client, {source}, run);
    CHECK(seen.front().bytes == 12000);
    CHECK(azl::UploadStates(file).find(hash) ==
          azl::UploadState{azl::UploadState::Kind::Uploaded, size, size, source.string(), "", 0});

    run.upload.progress = {};
    azl::runUpload(&client, {source}, run);
    CHECK(azl::UploadStates(file).find(hash)->kind == azl::UploadState::Kind::Uploaded);
    CHECK(azl::UploadStates(file).all().size() == 1);
}

TEST_CASE("an upload whose process is gone reads as failed; other processes' states are kept")
{
    TempDir dir;
    const fs::path file = dir.path / "states.json";
    const std::string a(64, 'a');
    const std::string b(64, 'b');
    const pid_t gone = fork();
    if (gone == 0)
        _exit(0);
    waitpid(gone, nullptr, 0);

    azl::UploadStates writer(file);
    azl::UploadStates other(file);
    writer.set(a, {.kind = azl::UploadState::Kind::Uploading, .bytes = 5, .size = 10, .pid = gone});
    other.set(b, {.kind = azl::UploadState::Kind::Uploading, .bytes = 1, .size = 10, .pid = getpid()});

    azl::UploadStates reader(file);
    REQUIRE(reader.all().size() == 2);
    CHECK(reader.find(a)->kind == azl::UploadState::Kind::Failed);
    CHECK(reader.find(a)->error == "interrupted: upload it again to resume");
    CHECK(reader.find(a)->bytes == 5);
    CHECK(reader.find(b)->kind == azl::UploadState::Kind::Uploading);
    CHECK_FALSE(reader.reload());
    writer.set(b, {.kind = azl::UploadState::Kind::Uploaded, .bytes = 10, .size = 10});
    CHECK(reader.reload());
    CHECK(reader.find(b)->kind == azl::UploadState::Kind::Uploaded);
    CHECK_FALSE(azl::UploadStates(file).find(std::string(64, 'c')));
}

TEST_CASE("ctl media upload sends a file up by hash; media status shows each clip's upload state")
{
    TempDir home;
    FakeS3 s3;
    const fs::path support = home.path / "Library/Application Support/Nocturne";
    const std::string local =
        azl::MediaLibrary(support / "MediaLibrary").add(copyMedia("count_100.mp4", home.path / "count_100.mp4")).hash;

    CtlRun status = ctlAt(home.path, s3, "media status");
    CHECK(status.status == 0);
    CHECK(status.out == local + "\tlocal only\tcount_100.mp4\n");

    if (ffmpegMissing())
        return;
    const fs::path source = mediaDir() / "count_240.mp4";
    const CtlRun upload = ctlAt(home.path, s3, "media upload '" + source.string() + "'");
    CHECK(upload.status == 0);
    const auto states = azl::UploadStates(support / "Uploads/states.json").all();
    REQUIRE(states.size() == 1);
    const std::string hash = states.begin()->first;
    CHECK(upload.out.find("uploaded  " + source.string() + " -> " + hash) != std::string::npos);
    CHECK(s3.objects().contains(hash));
    CHECK(fs::exists(support / "Uploads/converted"));

    status = ctlAt(home.path, s3, "media status");
    CHECK(status.out.find(local + "\tlocal only\tcount_100.mp4\n") != std::string::npos);
    CHECK(status.out.find(hash + "\tuploaded\t" + source.string() + "\n") != std::string::npos);

    const std::string failed(64, 'f');
    azl::UploadStates(support / "Uploads/states.json")
        .set(failed, {.kind = azl::UploadState::Kind::Failed, .bytes = 1500000, .size = 3000000, .error = "boom"});
    CHECK(ctlAt(home.path, s3, "media status " + failed).out == failed + "\tfailed 50% (1.5 of 3.0 MB): boom\t\n");
    const std::string elsewhere(64, 'e');
    CHECK(ctlAt(home.path, s3, "media status " + elsewhere).out == elsewhere + "\tnot on this machine\t\n");
    CHECK(ctlAt(home.path, s3, "media status nothex").status != 0);
}

TEST_SUITE_END();
