#include "engine/frame_trace.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

#include <mach/mach_time.h>

namespace azl {

double traceFromHost(double hostSeconds)
{
    static const double offset = [] {
        mach_timebase_info_data_t base;
        mach_timebase_info(&base);
        return traceNow() - double(mach_absolute_time()) * base.numer / base.denom * 1e-9;
    }();
    return hostSeconds + offset;
}

void FrameTrace::add(const Render& render)
{
    std::lock_guard lock(mutex_);
    if (renders_.size() < kMaxRecords)
        renders_.push_back(render);
}

void FrameTrace::add(const Present& present)
{
    std::lock_guard lock(mutex_);
    if (presents_.size() < kMaxRecords)
        presents_.push_back(present);
}

bool FrameTrace::write() const
{
    std::ofstream out(path_);
    if (!out)
        return false;
    std::lock_guard lock(mutex_);
    for (const Render& r : renders_) {
        const nlohmann::json line{{"k", "r"},        {"i", r.index},   {"t0", r.started},      {"stepped", r.stepped},
                                  {"updated", r.updated}, {"encoded", r.encoded}, {"gs", r.gpuStart}, {"ge", r.gpuEnd},
                                  {"done", r.done},  {"fading", r.fading}};
        out << line.dump() << '\n';
    }
    for (const Present& p : presents_) {
        const nlohmann::json line{{"k", "p"},          {"o", p.display},   {"vs", p.vsync},       {"woke", p.woke},
                                  {"targeted", p.targeted}, {"committed", p.committed}, {"gs", p.gpuStart},
                                  {"ge", p.gpuEnd},    {"shown", p.shown}, {"i", p.index},        {"new", p.fresh},
                                  {"t0", p.started}};
        out << line.dump() << '\n';
    }
    return bool(out);
}

} // namespace azl
