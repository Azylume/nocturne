#include "upload/upload_states.hpp"

#include "document/composition_file.hpp"
#include "media/media_library.hpp"
#include "platform/process.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>

namespace azl {

namespace fs = std::filesystem;

namespace {

constexpr const char* kInterrupted = "interrupted: upload it again to resume";

std::optional<fs::file_time_type> stampOf(const fs::path& file)
{
    std::error_code ec;
    const auto time = fs::last_write_time(file, ec);
    if (ec)
        return std::nullopt;
    return time;
}

std::optional<UploadState::Kind> kindFromName(const std::string& name)
{
    for (UploadState::Kind kind : {UploadState::Kind::Uploading, UploadState::Kind::Uploaded, UploadState::Kind::Failed})
        if (name == uploadStateName(kind))
            return kind;
    return std::nullopt;
}

UploadState resolved(UploadState state)
{
    if (state.kind == UploadState::Kind::Uploading && state.pid > 0 && !processRunning(state.pid)) {
        state.kind = UploadState::Kind::Failed;
        state.error = kInterrupted;
    }
    return state;
}

} // namespace

fs::path defaultUploadStatesFile()
{
    return MediaLibrary::defaultRoot().parent_path() / "Uploads" / "states.json";
}

const char* uploadStateName(UploadState::Kind kind)
{
    switch (kind) {
    case UploadState::Kind::Uploading: return "uploading";
    case UploadState::Kind::Uploaded: return "uploaded";
    case UploadState::Kind::Failed: return "failed";
    }
    return "";
}

UploadStates::UploadStates(fs::path file) : file_(std::move(file))
{
    reload();
}

bool UploadStates::reload()
{
    if (file_.empty())
        return false;
    const auto stamp = stampOf(file_);
    if (!stamp || *stamp == stamp_)
        return false;
    stamp_ = *stamp;
    load();
    return true;
}

std::optional<UploadState> UploadStates::find(const std::string& hash) const
{
    const auto it = states_.find(hash);
    if (it == states_.end())
        return std::nullopt;
    return resolved(it->second);
}

std::map<std::string, UploadState> UploadStates::all() const
{
    std::map<std::string, UploadState> result;
    for (const auto& [hash, state] : states_)
        result[hash] = resolved(state);
    return result;
}

void UploadStates::set(const std::string& hash, const UploadState& state)
{
    reload();
    states_[hash] = state;
    save();
}

void UploadStates::load()
{
    states_.clear();
    std::ifstream in(file_);
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object()) {
        std::fprintf(stderr, "upload states: unreadable %s\n", file_.string().c_str());
        return;
    }
    for (const auto& [hash, entry] : j.items()) {
        const auto kind = entry.is_object() ? kindFromName(entry.value("state", "")) : std::nullopt;
        if (kind)
            states_[hash] = {*kind, entry.value("bytes", uint64_t(0)), entry.value("size", uint64_t(0)),
                             entry.value("source", ""), entry.value("error", ""), entry.value("pid", 0)};
    }
}

void UploadStates::save()
{
    if (file_.empty())
        return;
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [hash, state] : states_) {
        nlohmann::json& entry = j[hash];
        entry = {{"state", uploadStateName(state.kind)}, {"bytes", state.bytes}, {"size", state.size},
                 {"source", state.source}};
        if (!state.error.empty())
            entry["error"] = state.error;
        if (state.kind == UploadState::Kind::Uploading)
            entry["pid"] = state.pid;
    }
    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    writeFileAtomically(file_, j.dump(2) + "\n", false);
    if (const auto stamp = stampOf(file_))
        stamp_ = *stamp;
}

} // namespace azl
