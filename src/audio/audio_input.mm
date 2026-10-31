#include "audio/audio_input.hpp"

#import <AVFoundation/AVFoundation.h>
#include <CoreAudio/AudioHardwareTapping.h>
#include <CoreAudio/CATapDescription.h>
#include <CoreAudio/CoreAudio.h>
#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>
#include <libproc.h>
#include <mach/mach_time.h>
#include <spawn.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>

extern "C" int responsibility_spawnattrs_setdisclaim(posix_spawnattr_t* attrs, int disclaim) __attribute__((weak_import));
extern char** environ;

namespace azl {

namespace {

using Clock = std::chrono::steady_clock;

constexpr UInt32 kBufferFrames = 256;
// A device that fails to start is tried again only after this, so the fallback is not restarted every second.
constexpr auto kRetryAfterFailure = std::chrono::seconds(10);
constexpr int kStalledTicks = 2;
constexpr double kMeterSeconds = 0.25;
constexpr std::array<AudioObjectPropertySelector, 3> kWatched = {
    kAudioHardwarePropertyDevices, kAudioHardwarePropertyDefaultInputDevice, kAudioHardwarePropertyProcessObjectList};

AudioObjectPropertyAddress address(AudioObjectPropertySelector selector,
                                   AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    return {selector, scope, kAudioObjectPropertyElementMain};
}

template <typename T>
bool get(AudioObjectID object, AudioObjectPropertyAddress where, T& value)
{
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(object, &where, 0, nullptr, &size, &value) == noErr;
}

std::string stringProperty(AudioObjectID object, AudioObjectPropertySelector selector)
{
    CFStringRef value = nullptr;
    if (!get(object, address(selector), value) || !value)
        return {};
    std::array<char, 512> buffer{};
    std::string result;
    if (CFStringGetCString(value, buffer.data(), buffer.size(), kCFStringEncodingUTF8))
        result = buffer.data();
    CFRelease(value);
    return result;
}

std::vector<AudioObjectID> deviceIds()
{
    auto where = address(kAudioHardwarePropertyDevices);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &where, 0, nullptr, &size) != noErr)
        return {};
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &where, 0, nullptr, &size, ids.data()) != noErr)
        return {};
    ids.resize(size / sizeof(AudioObjectID));
    return ids;
}

// Channels per input buffer, in the order the IOProc receives them.
std::vector<UInt32> inputLayout(AudioObjectID device)
{
    auto where = address(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &where, 0, nullptr, &size) != noErr || size < sizeof(AudioBufferList))
        return {};
    std::vector<uint8_t> storage(size);
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &where, 0, nullptr, &size, list) != noErr)
        return {};
    std::vector<UInt32> layout;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
        layout.push_back(list->mBuffers[i].mNumberChannels);
    return layout;
}

int channelCount(AudioObjectID device)
{
    int total = 0;
    for (UInt32 n : inputLayout(device))
        total += int(n);
    return total;
}

AudioObjectID defaultInput()
{
    AudioObjectID id = kAudioObjectUnknown;
    get(kAudioObjectSystemObject, address(kAudioHardwarePropertyDefaultInputDevice), id);
    return id;
}

double nominalRate(AudioObjectID device)
{
    Float64 rate = 0.0;
    get(device, address(kAudioDevicePropertyNominalSampleRate), rate);
    return rate;
}

bool alive(AudioObjectID device)
{
    UInt32 value = 0;
    return get(device, address(kAudioDevicePropertyDeviceIsAlive), value) && value;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// A UID first, then an exact name, then a case-insensitive part of a name.
AudioObjectID find(const std::string& requested)
{
    std::vector<std::pair<AudioObjectID, std::string>> inputs;
    for (AudioObjectID id : deviceIds())
        if (channelCount(id) > 0) {
            if (stringProperty(id, kAudioDevicePropertyDeviceUID) == requested)
                return id;
            inputs.emplace_back(id, stringProperty(id, kAudioObjectPropertyName));
        }
    for (const auto& [id, name] : inputs)
        if (name == requested)
            return id;
    for (const auto& [id, name] : inputs)
        if (lower(name).find(lower(requested)) != std::string::npos)
            return id;
    return kAudioObjectUnknown;
}

std::string permissionName(AVAuthorizationStatus status)
{
    switch (status) {
    case AVAuthorizationStatusAuthorized: return "authorized";
    case AVAuthorizationStatusDenied: return "denied";
    case AVAuthorizationStatusRestricted: return "restricted";
    default: return "notDetermined";
    }
}

// What to capture: an input device, or a process tap of the whole system (no processes) or of some processes.
struct Target {
    AudioObjectID device = kAudioObjectUnknown;
    bool tap = false;
    std::vector<AudioObjectID> processes;

    static Target input(AudioObjectID device) { return {device, false, {}}; }
    static Target system() { return {kAudioObjectUnknown, true, {}}; }

    bool valid() const { return tap || device != kAudioObjectUnknown; }
    bool operator==(const Target&) const = default;
};

// One running IOProc on one device. The callback reads only this object, which outlives the IOProc.
struct Session {
    AudioAnalysis* analysis = nullptr;
    Target target;
    // The device the IOProc runs on: the input device, or the aggregate device that carries the tap.
    AudioObjectID device = kAudioObjectUnknown;
    AudioObjectID tap = kAudioObjectUnknown;
    AudioDeviceIOProcID proc = nullptr;
    double sampleRate = 0.0;
    std::string uid;
    std::string name;
    // The buffer and channel within it of each mixed channel.
    std::array<std::pair<UInt32, UInt32>, 64> taps{};
    size_t tapCount = 0;
    float scale = 1.0f;
    std::array<float, 4096> mono{};
    double ticksToSeconds = 0.0;
    std::atomic<uint64_t> callbacks{0};
    std::array<ChannelMeter, AudioAnalysis::kMaxChannels> meters{};
    std::array<ChannelLevel, AudioAnalysis::kMaxChannels> levels{};

    ~Session()
    {
        if (proc) {
            AudioDeviceStop(device, proc);
            AudioDeviceDestroyIOProcID(device, proc);
        }
        if (analysis)
            analysis->setChannels(nullptr, 0);
        if (@available(macOS 14.2, *)) {
            if (target.tap && device != kAudioObjectUnknown)
                AudioHardwareDestroyAggregateDevice(device);
            if (tap != kAudioObjectUnknown)
                AudioHardwareDestroyProcessTap(tap);
        }
    }
};

// Every input channel of the device, mixed or not, a few times a second.
void measure(Session& s, const AudioBufferList& input, UInt32 frames)
{
    size_t channel = 0;
    for (UInt32 b = 0; b < input.mNumberBuffers; ++b) {
        const AudioBuffer& buffer = input.mBuffers[b];
        if (!buffer.mData || buffer.mNumberChannels == 0)
            return;
        const auto* data = static_cast<const float*>(buffer.mData);
        const UInt32 n = std::min<UInt32>(frames, buffer.mDataByteSize / (sizeof(float) * buffer.mNumberChannels));
        for (UInt32 c = 0; c < buffer.mNumberChannels && channel < s.meters.size(); ++c, ++channel)
            for (UInt32 i = 0; i < n; ++i)
                s.meters[channel].add(data[size_t(i) * buffer.mNumberChannels + c]);
    }
    if (channel == 0 || double(s.meters[0].count) < s.sampleRate * kMeterSeconds)
        return;
    for (size_t c = 0; c < channel; ++c)
        s.levels[c] = s.meters[c].take();
    s.analysis->setChannels(s.levels.data(), channel);
}

OSStatus capture(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* input, const AudioTimeStamp* inputTime,
                 AudioBufferList*, const AudioTimeStamp*, void* client)
{
    auto* s = static_cast<Session*>(client);
    s->callbacks.fetch_add(1, std::memory_order_relaxed);
    if (!input || s->tapCount == 0)
        return noErr;
    UInt32 frames = UINT32_MAX;
    for (size_t t = 0; t < s->tapCount; ++t) {
        const auto [buffer, channel] = s->taps[t];
        if (buffer >= input->mNumberBuffers || channel >= input->mBuffers[buffer].mNumberChannels ||
            !input->mBuffers[buffer].mData)
            return noErr;
        const AudioBuffer& b = input->mBuffers[buffer];
        frames = std::min<UInt32>(frames, b.mDataByteSize / (sizeof(float) * b.mNumberChannels));
    }
    double time = s->analysis->now();
    if (inputTime && (inputTime->mFlags & kAudioTimeStampHostTimeValid))
        time -= double(int64_t(mach_absolute_time() - inputTime->mHostTime)) * s->ticksToSeconds;
    for (UInt32 done = 0; done < frames;) {
        const UInt32 n = std::min<UInt32>(frames - done, UInt32(s->mono.size()));
        for (UInt32 i = 0; i < n; ++i) {
            float sum = 0.0f;
            for (size_t t = 0; t < s->tapCount; ++t) {
                const AudioBuffer& b = input->mBuffers[s->taps[t].first];
                sum += static_cast<const float*>(b.mData)[size_t(done + i) * b.mNumberChannels + s->taps[t].second];
            }
            s->mono[i] = sum * s->scale;
        }
        s->analysis->push(s->mono.data(), n, time + double(done) / s->sampleRate, s->sampleRate);
        done += n;
    }
    measure(*s, *input, frames);
    return noErr;
}

void setBufferFrames(AudioObjectID device)
{
    AudioValueRange range{};
    if (!get(device, address(kAudioDevicePropertyBufferFrameSizeRange), range))
        return;
    UInt32 frames = UInt32(std::clamp<double>(kBufferFrames, range.mMinimum, range.mMaximum));
    auto where = address(kAudioDevicePropertyBufferFrameSize);
    AudioObjectSetPropertyData(device, &where, 0, nullptr, sizeof(frames), &frames);
}

std::vector<std::pair<UInt32, UInt32>> allChannels(AudioObjectID device)
{
    std::vector<std::pair<UInt32, UInt32>> all;
    const std::vector<UInt32> layout = inputLayout(device);
    for (UInt32 b = 0; b < layout.size(); ++b)
        for (UInt32 c = 0; c < layout[b]; ++c)
            all.emplace_back(b, c);
    return all;
}

std::vector<AudioObjectID> processObjects()
{
    auto where = address(kAudioHardwarePropertyProcessObjectList);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &where, 0, nullptr, &size) != noErr)
        return {};
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &where, 0, nullptr, &size, ids.data()) != noErr)
        return {};
    ids.resize(size / sizeof(AudioObjectID));
    return ids;
}

AudioAppInfo appInfo(AudioObjectID process)
{
    pid_t pid = -1;
    get(process, address(kAudioProcessPropertyPID), pid);
    UInt32 playing = 0;
    get(process, address(kAudioProcessPropertyIsRunningOutput), playing);
    std::array<char, 256> name{};
    if (pid > 0)
        proc_name(pid, name.data(), uint32_t(name.size()));
    return {int(pid), stringProperty(process, kAudioProcessPropertyBundleID), name.data(), playing != 0};
}

// Processes whose bundle ID or name contains `query` (case-insensitive), or whose pid it is.
std::vector<AudioObjectID> matchProcesses(const std::string& query)
{
    std::vector<AudioObjectID> matched;
    for (AudioObjectID process : processObjects()) {
        const AudioAppInfo app = appInfo(process);
        if (app.pid == getpid())
            continue;
        if (std::to_string(app.pid) == query || lower(app.bundle).find(lower(query)) != std::string::npos ||
            lower(app.name).find(lower(query)) != std::string::npos)
            matched.push_back(process);
    }
    std::sort(matched.begin(), matched.end());
    return matched;
}

constexpr std::string_view kAppPrefix = "app:";

Target resolve(const std::string& requested)
{
    if (requested.empty())
        return Target::input(defaultInput());
    if (requested == "system")
        return Target::system();
    if (requested.starts_with(kAppPrefix)) {
        std::vector<AudioObjectID> processes = matchProcesses(requested.substr(kAppPrefix.size()));
        if (processes.empty())
            return {};
        return {.tap = true, .processes = std::move(processes)};
    }
    return Target::input(find(requested));
}

// A private process tap and a private aggregate device that carries it, clocked by the default output device. The
// tap's channels are the aggregate's last input channels.
bool createTap(Session& s, std::string& error)
{
    if (@available(macOS 14.2, *)) {
        NSMutableArray<NSNumber*>* processes = [NSMutableArray array];
        for (AudioObjectID process : s.target.processes)
            [processes addObject:@(process)];
        CATapDescription* description =
            s.target.processes.empty() ? [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[]]
                                       : [[CATapDescription alloc] initStereoMixdownOfProcesses:processes];
        description.name = @"Nocturne";
        description.privateTap = YES;
        description.muteBehavior = CATapUnmuted;
        if (OSStatus e = AudioHardwareCreateProcessTap(description, &s.tap); e != noErr) {
            s.tap = kAudioObjectUnknown;
            error = "cannot create the process tap (" + std::to_string(e) + ")";
            return false;
        }
        AudioObjectID output = kAudioObjectUnknown;
        get(kAudioObjectSystemObject, address(kAudioHardwarePropertyDefaultOutputDevice), output);
        NSString* outputUid =
            [NSString stringWithUTF8String:stringProperty(output, kAudioDevicePropertyDeviceUID).c_str()];
        NSDictionary* aggregate = @{
            @kAudioAggregateDeviceNameKey : @"Nocturne Tap",
            @kAudioAggregateDeviceUIDKey : [NSUUID UUID].UUIDString,
            @kAudioAggregateDeviceMainSubDeviceKey : outputUid,
            @kAudioAggregateDeviceIsPrivateKey : @YES,
            @kAudioAggregateDeviceIsStackedKey : @NO,
            @kAudioAggregateDeviceTapAutoStartKey : @YES,
            @kAudioAggregateDeviceSubDeviceListKey : @[ @{@kAudioSubDeviceUIDKey : outputUid} ],
            @kAudioAggregateDeviceTapListKey :
                @[ @{@kAudioSubTapDriftCompensationKey : @YES, @kAudioSubTapUIDKey : description.UUID.UUIDString} ],
        };
        if (OSStatus e = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggregate, &s.device);
            e != noErr) {
            s.device = kAudioObjectUnknown;
            error = "cannot create the aggregate device for the tap (" + std::to_string(e) + ")";
            return false;
        }
        AudioStreamBasicDescription format{};
        get(s.tap, address(kAudioTapPropertyFormat), format);
        const auto all = allChannels(s.device);
        const size_t channels = std::min<size_t>(std::max<UInt32>(format.mChannelsPerFrame, 1), all.size());
        for (size_t i = all.size() - channels; i < all.size() && s.tapCount < s.taps.size(); ++i)
            s.taps[s.tapCount++] = all[i];
        std::string names;
        for (AudioObjectID process : s.target.processes) {
            const AudioAppInfo app = appInfo(process);
            names += (names.empty() ? "" : ", ") + (app.name.empty() ? app.bundle : app.name);
        }
        s.name = s.target.processes.empty() ? "system audio" : "app audio (" + names + ")";
        s.uid = s.target.processes.empty() ? "system" : "app";
        return true;
    }
    error = "process taps need macOS 14.2";
    return false;
}

void selectChannels(Session& s, const std::vector<int>& channels)
{
    const auto all = allChannels(s.device);
    for (int channel : channels)
        if (channel >= 1 && size_t(channel) <= all.size() && s.tapCount < s.taps.size())
            s.taps[s.tapCount++] = all[size_t(channel - 1)];
    if (s.tapCount == 0)
        for (size_t i = 0; i < all.size() && i < s.taps.size(); ++i)
            s.taps[s.tapCount++] = all[i];
    s.uid = stringProperty(s.device, kAudioDevicePropertyDeviceUID);
    s.name = stringProperty(s.device, kAudioObjectPropertyName);
}

std::unique_ptr<Session> start(const Target& target, const std::vector<int>& channels, AudioAnalysis& analysis,
                               std::string& error)
{
    auto s = std::make_unique<Session>();
    s->analysis = &analysis;
    s->target = target;
    mach_timebase_info_data_t timebase{};
    mach_timebase_info(&timebase);
    s->ticksToSeconds = double(timebase.numer) / double(timebase.denom) / 1e9;
    if (target.tap) {
        if (!createTap(*s, error))
            return nullptr;
    } else {
        s->device = target.device;
        selectChannels(*s, channels);
    }
    s->sampleRate = nominalRate(s->device);
    if (s->tapCount == 0 || s->sampleRate <= 0.0) {
        error = "no input channels";
        return nullptr;
    }
    s->scale = 1.0f / float(s->tapCount);

    setBufferFrames(s->device);
    if (OSStatus e = AudioDeviceCreateIOProcID(s->device, capture, s.get(), &s->proc); e != noErr) {
        s->proc = nullptr;
        error = "cannot create the IOProc (" + std::to_string(e) + ")";
        return nullptr;
    }
    if (OSStatus e = AudioDeviceStart(s->device, s->proc); e != noErr) {
        error = "cannot start the device (" + std::to_string(e) + ")";
        return nullptr;
    }
    return s;
}

bool sameStatus(const AudioInputStatus& a, const AudioInputStatus& b)
{
    return a.requested == b.requested && a.channels == b.channels && a.uid == b.uid && a.name == b.name &&
           a.sampleRate == b.sampleRate && a.state == b.state && a.permission == b.permission &&
           a.restarts == b.restarts;
}

} // namespace

struct AudioInput::Impl : std::enable_shared_from_this<AudioInput::Impl> {
    explicit Impl(AudioAnalysis& analysis) : analysis(analysis) {}

    void reconcile(const std::string& why, bool force = false);
    void stop() { session.reset(); }
    void publish(AudioInputStatus next, const std::string& event);
    void tick();
    void askPermission(bool request);
    AudioInputStatus current() const
    {
        std::lock_guard lock(mutex);
        return status;
    }

    AudioAnalysis& analysis;
    std::mutex changedMutex;
    Changed changed;
    std::function<void()> devicesChanged;
    dispatch_queue_t queue = dispatch_queue_create("nocturne.audio.input", DISPATCH_QUEUE_SERIAL);
    dispatch_source_t timer = nullptr;
    AudioObjectPropertyListenerBlock listener = nullptr;

    // Queue only.
    bool closed = false;
    bool askedPermission = false;
    std::string requested;
    std::vector<int> channels;
    std::unique_ptr<Session> session;
    Target failed;
    Clock::time_point retryAt;
    uint64_t lastCallbacks = 0;
    int stalled = 0;

    mutable std::mutex mutex;
    AudioInputStatus status;
};

void AudioInput::Impl::publish(AudioInputStatus next, const std::string& event)
{
    {
        std::lock_guard lock(mutex);
        if (sameStatus(status, next) && event.empty())
            return;
        status = next;
    }
    std::lock_guard lock(changedMutex);
    if (changed)
        changed(next, event);
}

void AudioInput::Impl::reconcile(const std::string& why, bool force)
{
    if (!askedPermission && (requested != "off" || current().permission.empty()))
        askPermission(requested != "off");
    AudioInputStatus next = current();
    next.requested = requested;
    next.channels = channels;
    if (requested == "off") {
        const bool wasRunning = session != nullptr;
        stop();
        next.uid = next.name = {};
        next.sampleRate = 0.0;
        next.state = "off";
        return publish(next, wasRunning || why == "select" ? why + ": input off" : "");
    }

    Target target = resolve(requested);
    bool fallback = false;
    const bool backingOff = target.valid() && target == failed && Clock::now() < retryAt;
    if ((!target.valid() && !requested.empty()) || backingOff) {
        target = Target::input(defaultInput());
        fallback = true;
    }
    const bool healthy = session && session->target == target && !force && alive(session->device) &&
                         nominalRate(session->device) == session->sampleRate;
    if (healthy) {
        next.state = fallback ? "fallback" : "running";
        return publish(next, "");
    }

    const bool restart = session && session->target == target;
    stop();
    std::string error;
    if (target.valid()) {
        session = start(target, channels, analysis, error);
        if (!session) {
            failed = target;
            retryAt = Clock::now() + kRetryAfterFailure;
        }
    }
    const Target fallbackTarget = Target::input(defaultInput());
    if (!session && target != fallbackTarget && fallbackTarget.valid()) {
        fallback = true;
        session = start(fallbackTarget, channels, analysis, error);
    }
    lastCallbacks = 0;
    stalled = 0;
    if (!session) {
        const bool quiet = current().state == "none" && why != "select";
        next.uid = next.name = {};
        next.sampleRate = 0.0;
        next.state = "none";
        return publish(next, quiet ? "" : why + ": no input device" + (error.empty() ? "" : " (" + error + ")"));
    }
    if (restart && why != "select")
        ++next.restarts;
    next.uid = session->uid;
    next.name = session->name;
    next.sampleRate = session->sampleRate;
    next.state = fallback ? "fallback" : "running";
    std::string event = why + ": capturing " + session->name + " at " + std::to_string(int(session->sampleRate)) + " Hz";
    if (fallback && !requested.empty())
        event += " ('" + requested + "' is not available)";
    publish(next, event);
}

void AudioInput::Impl::tick()
{
    if (session) {
        const uint64_t callbacks = session->callbacks.load(std::memory_order_relaxed);
        stalled = callbacks == lastCallbacks ? stalled + 1 : 0;
        lastCallbacks = callbacks;
        if (stalled >= kStalledTicks)
            return reconcile("callbacks stopped", true);
    }
    reconcile("device change");
}

void AudioInput::Impl::askPermission(bool request)
{
    AudioInputStatus next = current();
    next.permission = AudioInput::permission();
    publish(next, "microphone permission: " + next.permission);
    if (!request)
        return;
    askedPermission = true;
    if (next.permission != "notDetermined" ||
        ![[NSBundle mainBundle] objectForInfoDictionaryKey:@"NSMicrophoneUsageDescription"])
        return;
    // The answer may come after the input is gone.
    std::shared_ptr<Impl> self = shared_from_this();
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                             completionHandler:^(BOOL) {
                               dispatch_async(self->queue, ^{
                                 if (self->closed)
                                     return;
                                 AudioInputStatus asked = self->current();
                                 asked.permission = AudioInput::permission();
                                 self->publish(asked, "microphone permission: " + asked.permission);
                                 self->reconcile("permission", true);
                               });
                             }];
}

AudioInput::AudioInput(AudioAnalysis& analysis) : impl_(std::make_shared<Impl>(analysis))
{
    Impl* impl = impl_.get();
    if (disabled())
        impl->requested = "off";
    impl->listener = ^(UInt32 count, const AudioObjectPropertyAddress* addresses) {
      if (std::any_of(addresses, addresses + count, [](const AudioObjectPropertyAddress& a) {
              return a.mSelector == kAudioHardwarePropertyDevices;
          })) {
          std::lock_guard lock(impl->changedMutex);
          if (impl->devicesChanged)
              impl->devicesChanged();
      }
      impl->reconcile("device change");
    };
    for (AudioObjectPropertySelector selector : kWatched) {
        auto where = address(selector);
        AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &where, impl->queue, impl->listener);
    }

    impl->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, impl->queue);
    dispatch_source_set_timer(impl->timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC,
                              NSEC_PER_SEC / 10);
    dispatch_source_set_event_handler(impl->timer, ^{ impl->tick(); });
    dispatch_resume(impl->timer);
}

AudioInput::~AudioInput()
{
    Impl* impl = impl_.get();
    for (AudioObjectPropertySelector selector : kWatched) {
        auto where = address(selector);
        AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &where, impl->queue, impl->listener);
    }
    dispatch_sync(impl->queue, ^{
      impl->closed = true;
      dispatch_source_cancel(impl->timer);
      impl->stop();
    });
    dispatch_sync(impl->queue, ^{});
}

std::vector<AudioDeviceInfo> AudioInput::devices()
{
    const AudioObjectID fallback = defaultInput();
    std::vector<AudioDeviceInfo> result;
    for (AudioObjectID id : deviceIds()) {
        const int channels = channelCount(id);
        if (channels == 0)
            continue;
        result.push_back({stringProperty(id, kAudioDevicePropertyDeviceUID), stringProperty(id, kAudioObjectPropertyName),
                          channels, nominalRate(id), id == fallback});
    }
    return result;
}

std::vector<AudioAppInfo> AudioInput::apps()
{
    std::vector<AudioAppInfo> result;
    for (AudioObjectID process : processObjects())
        if (AudioAppInfo app = appInfo(process); app.pid != getpid())
            result.push_back(app);
    return result;
}

std::string AudioInput::permission()
{
    return permissionName([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]);
}

void AudioInput::select(std::string device, std::vector<int> channels)
{
    Impl* impl = impl_.get();
    if (disabled())
        device = "off";
    auto request = std::make_shared<std::pair<std::string, std::vector<int>>>(std::move(device), std::move(channels));
    dispatch_async(impl->queue, ^{
      impl->requested = request->first;
      impl->channels = request->second;
      impl->reconcile("select", true);
    });
}

void AudioInput::onChange(Changed changed)
{
    std::lock_guard lock(impl_->changedMutex);
    impl_->changed = std::move(changed);
}

void AudioInput::onDevicesChange(std::function<void()> changed)
{
    std::lock_guard lock(impl_->changedMutex);
    impl_->devicesChanged = std::move(changed);
}

AudioInputStatus AudioInput::status() const
{
    return impl_->current();
}

void becomeResponsibleProcess(char** argv)
{
    if (std::getenv("NOCTURNE_RESPONSIBLE") || !responsibility_spawnattrs_setdisclaim)
        return;
    std::array<char, PATH_MAX> path{};
    uint32_t size = path.size();
    if (_NSGetExecutablePath(path.data(), &size) != 0)
        return;
    setenv("NOCTURNE_RESPONSIBLE", "1", 1);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETEXEC);
    responsibility_spawnattrs_setdisclaim(&attributes, 1);
    posix_spawn(nullptr, path.data(), nullptr, &attributes, argv, environ);
    posix_spawnattr_destroy(&attributes);
}

} // namespace azl
