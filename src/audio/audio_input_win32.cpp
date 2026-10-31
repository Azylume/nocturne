#include "audio/audio_input.hpp"

#include <windows.h>

#include <objbase.h>
#include <propidl.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <initguid.h>

#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <thread>

namespace azl {

namespace {

using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;

// A device that fails to start is tried again only after this, so the fallback is not restarted every second.
constexpr auto kRetryAfterFailure = std::chrono::seconds(10);
constexpr int kStalledTicks = 2;
constexpr double kMeterSeconds = 0.25;
constexpr REFERENCE_TIME kBufferDuration = 200000;
constexpr DWORD kWaitMs = 100;
constexpr DWORD kActivateTimeoutMs = 5000;
// Process loopback has no format of its own: it delivers what it is opened with.
constexpr WORD kTapChannels = 2;
constexpr DWORD kTapRate = 48000;
constexpr std::string_view kAppPrefix = "app:";
constexpr GUID kFloatSubFormat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

std::string utf8(const wchar_t* text)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string result(size_t(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring wide(const std::string& text)
{
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 1)
        return {};
    std::wstring result(size_t(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), size);
    return result;
}

// COM for the calling thread while it lives; a thread that already has another apartment keeps it.
class ComScope {
public:
    ComScope() : owned_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComScope()
    {
        if (owned_)
            CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    bool owned_;
};

ComPtr<IMMDeviceEnumerator> enumerator()
{
    ComPtr<IMMDeviceEnumerator> result;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&result));
    return result;
}

std::vector<ComPtr<IMMDevice>> endpoints(EDataFlow flow)
{
    std::vector<ComPtr<IMMDevice>> result;
    ComPtr<IMMDeviceCollection> collection;
    const ComPtr<IMMDeviceEnumerator> devices = enumerator();
    if (!devices || FAILED(devices->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection)))
        return result;
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(collection->Item(i, &device)))
            result.push_back(device);
    }
    return result;
}

std::string endpointId(IMMDevice* device)
{
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)))
        return {};
    std::string result = utf8(id);
    CoTaskMemFree(id);
    return result;
}

std::string friendlyName(IMMDevice* device)
{
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store)))
        return {};
    PROPVARIANT value;
    PropVariantInit(&value);
    std::string result;
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
        result = utf8(value.pwszVal);
    PropVariantClear(&value);
    return result;
}

ComPtr<IMMDevice> endpoint(const std::string& id)
{
    ComPtr<IMMDevice> device;
    const ComPtr<IMMDeviceEnumerator> devices = enumerator();
    if (devices)
        devices->GetDevice(wide(id).c_str(), &device);
    return device;
}

std::string defaultEndpoint(EDataFlow flow)
{
    ComPtr<IMMDevice> device;
    const ComPtr<IMMDeviceEnumerator> devices = enumerator();
    if (!devices || FAILED(devices->GetDefaultAudioEndpoint(flow, eConsole, &device)))
        return {};
    return endpointId(device.Get());
}

// The shared-mode format, as 32-bit float with the same channels and rate.
struct MixFormat {
    std::vector<uint8_t> bytes;
    const WAVEFORMATEX* get() const { return reinterpret_cast<const WAVEFORMATEX*>(bytes.data()); }
};

MixFormat floatMix(IAudioClient* client)
{
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)))
        return {};
    MixFormat result{std::vector<uint8_t>(reinterpret_cast<uint8_t*>(mix),
                                          reinterpret_cast<uint8_t*>(mix) + sizeof(WAVEFORMATEX) + mix->cbSize)};
    CoTaskMemFree(mix);
    auto* format = reinterpret_cast<WAVEFORMATEX*>(result.bytes.data());
    format->wBitsPerSample = 32;
    format->nBlockAlign = WORD(format->nChannels * 4);
    format->nAvgBytesPerSec = format->nSamplesPerSec * format->nBlockAlign;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
        auto* extensible = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format);
        extensible->Samples.wValidBitsPerSample = 32;
        extensible->SubFormat = kFloatSubFormat;
    } else {
        format->wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format->cbSize = 0;
        result.bytes.resize(sizeof(WAVEFORMATEX));
    }
    return result;
}

WAVEFORMATEX tapFormat()
{
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = kTapChannels;
    format.nSamplesPerSec = kTapRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = WORD(kTapChannels * 4);
    format.nAvgBytesPerSec = kTapRate * format.nBlockAlign;
    return format;
}

ComPtr<IAudioClient> activate(IMMDevice* device)
{
    ComPtr<IAudioClient> client;
    if (device)
        device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    return client;
}

// A UID first, then an exact name, then a case-insensitive part of a name.
std::string findInput(const std::string& requested)
{
    std::vector<std::pair<std::string, std::string>> inputs;
    for (const ComPtr<IMMDevice>& device : endpoints(eCapture)) {
        std::string id = endpointId(device.Get());
        if (id == requested)
            return id;
        inputs.emplace_back(std::move(id), friendlyName(device.Get()));
    }
    for (const auto& [id, name] : inputs)
        if (name == requested)
            return id;
    for (const auto& [id, name] : inputs)
        if (containsIgnoringCase(name, requested))
            return id;
    return {};
}

std::string permissionName()
{
    constexpr const wchar_t* kKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\"
                                    L"microphone";
    const auto denied = [](HKEY root, const std::wstring& key) {
        std::array<wchar_t, 16> value{};
        DWORD size = DWORD(value.size() * sizeof(wchar_t));
        return RegGetValueW(root, key.c_str(), L"Value", RRF_RT_REG_SZ, nullptr, value.data(), &size) == ERROR_SUCCESS &&
               std::wstring(value.data()) == L"Deny";
    };
    if (denied(HKEY_LOCAL_MACHINE, kKey))
        return "restricted";
    if (denied(HKEY_CURRENT_USER, kKey) || denied(HKEY_CURRENT_USER, std::wstring(kKey) + L"\\NonPackaged"))
        return "denied";
    return "authorized";
}

struct Process {
    DWORD pid = 0;
    DWORD parent = 0;
    std::string exe;
};

std::vector<Process> processes()
{
    std::vector<Process> result;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return result;
    PROCESSENTRY32W entry{.dwSize = sizeof(PROCESSENTRY32W)};
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
        result.push_back({entry.th32ProcessID, entry.th32ParentProcessID, utf8(entry.szExeFile)});
    CloseHandle(snapshot);
    return result;
}

AudioAppInfo appInfo(const Process& process, bool playing)
{
    std::string name = process.exe;
    if (name.size() > 4 && containsIgnoringCase(name.substr(name.size() - 4), ".exe"))
        name.resize(name.size() - 4);
    return {int(process.pid), process.exe, name, playing};
}

// The process whose executable contains `query` (case-insensitive), or whose pid it is, highest in its process tree,
// so its loopback also carries the child process that plays its audio. Zero when none matches.
DWORD matchProcess(const std::string& query)
{
    const std::vector<Process> all = processes();
    std::map<DWORD, DWORD> matched;
    for (const Process& p : all)
        if (p.pid != GetCurrentProcessId() && (std::to_string(p.pid) == query || containsIgnoringCase(p.exe, query)))
            matched[p.pid] = p.parent;
    for (const auto& [pid, parent] : matched)
        if (!matched.contains(parent))
            return pid;
    return 0;
}

// What to capture: an input endpoint, all system audio, or one app's process tree.
struct Target {
    enum class Kind { None, Device, System, App };
    Kind kind = Kind::None;
    std::string device;
    DWORD pid = 0;

    static Target input(std::string id) { return id.empty() ? Target{} : Target{Kind::Device, std::move(id), 0}; }

    bool valid() const { return kind != Kind::None; }
    bool tap() const { return kind == Kind::System || kind == Kind::App; }
    bool operator==(const Target&) const = default;
};

Target resolve(const std::string& requested)
{
    if (requested.empty())
        return Target::input(defaultEndpoint(eCapture));
    if (requested == "system")
        return {Target::Kind::System, {}, 0};
    if (requested.starts_with(kAppPrefix)) {
        const DWORD pid = matchProcess(requested.substr(kAppPrefix.size()));
        return pid ? Target{Target::Kind::App, {}, pid} : Target{};
    }
    return Target::input(findInput(requested));
}

// One running capture stream. Its thread reads only this object, which outlives the thread.
struct Session {
    AudioAnalysis* analysis = nullptr;
    Target target;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    double sampleRate = 0.0;
    size_t channels = 0;
    std::string uid;
    std::string name;
    std::array<size_t, 64> taps{};
    size_t tapCount = 0;
    float scale = 1.0f;
    std::array<float, 4096> mono{};
    std::array<ChannelMeter, AudioAnalysis::kMaxChannels> meters{};
    std::array<ChannelLevel, AudioAnalysis::kMaxChannels> levels{};
    std::atomic<uint64_t> callbacks{0};
    std::atomic<bool> failed{false};
    std::atomic<bool> stopping{false};
    std::thread thread;

    ~Session()
    {
        stopping = true;
        SetEvent(event);
        if (thread.joinable())
            thread.join();
        if (client)
            client->Stop();
        if (analysis)
            analysis->setChannels(nullptr, 0);
        CloseHandle(event);
    }
};

// The performance counter in seconds: the clock WASAPI stamps packets with.
double counterSeconds()
{
    static const double period = [] {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return 1.0 / double(frequency.QuadPart);
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return double(counter.QuadPart) * period;
}

// Every channel of the stream, mixed or not, a few times a second.
void measure(Session& s, const float* data, UINT32 frames)
{
    const size_t channels = std::min(s.channels, s.meters.size());
    for (UINT32 i = 0; i < frames; ++i)
        for (size_t c = 0; c < channels; ++c)
            s.meters[c].add(data ? data[size_t(i) * s.channels + c] : 0.0f);
    if (channels == 0 || double(s.meters[0].count) < s.sampleRate * kMeterSeconds)
        return;
    for (size_t c = 0; c < channels; ++c)
        s.levels[c] = s.meters[c].take();
    s.analysis->setChannels(s.levels.data(), channels);
}

// `data` is null for a silent packet. `position` is the first frame's capture time in 100 ns QPC units, or 0.
void deliver(Session& s, const float* data, UINT32 frames, UINT64 position)
{
    double time = s.analysis->now();
    if (position)
        time -= counterSeconds() - double(position) * 1e-7;
    for (UINT32 done = 0; done < frames;) {
        const UINT32 n = std::min<UINT32>(frames - done, UINT32(s.mono.size()));
        for (UINT32 i = 0; i < n; ++i) {
            float sum = 0.0f;
            if (data)
                for (size_t t = 0; t < s.tapCount; ++t)
                    sum += data[size_t(done + i) * s.channels + s.taps[t]];
            s.mono[i] = sum * s.scale;
        }
        s.analysis->push(s.mono.data(), n, time + double(done) / s.sampleRate, s.sampleRate);
        done += n;
    }
    measure(s, data, frames);
}

// False when the stream broke, as when its device is unplugged or its format changes.
bool drain(Session& s)
{
    UINT32 packet = 0;
    for (;;) {
        if (FAILED(s.capture->GetNextPacketSize(&packet)))
            return false;
        if (packet == 0)
            return true;
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 position = 0;
        if (FAILED(s.capture->GetBuffer(&data, &frames, &flags, nullptr, &position)))
            return false;
        const bool silent = flags & AUDCLNT_BUFFERFLAGS_SILENT;
        const bool timed = !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR);
        deliver(s, silent ? nullptr : reinterpret_cast<const float*>(data), frames, timed ? position : 0);
        if (FAILED(s.capture->ReleaseBuffer(frames)))
            return false;
    }
}

// The capture thread. A loopback stream gets no packets while nothing plays, so for a tap every wait counts as alive.
void run(Session& s)
{
    ComScope com;
    DWORD task = 0;
    const HANDLE priority = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    while (!s.stopping) {
        const DWORD woke = WaitForSingleObject(s.event, kWaitMs);
        if (s.stopping)
            break;
        if (woke == WAIT_OBJECT_0 || s.target.tap())
            s.callbacks.fetch_add(1, std::memory_order_relaxed);
        if (!drain(s)) {
            s.failed = true;
            break;
        }
    }
    if (priority)
        AvRevertMmThreadCharacteristics(priority);
}

// Waits for ActivateAudioInterfaceAsync. Agile, as the call requires.
class Activation final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    ~Activation() { CloseHandle(done); }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --references_;
        if (left == 0)
            delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override
    {
        if (id == __uuidof(IUnknown) || id == __uuidof(IActivateAudioInterfaceCompletionHandler))
            *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (id == __uuidof(IAgileObject))
            *object = static_cast<IAgileObject*>(this);
        else {
            *object = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override
    {
        SetEvent(done);
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1};
};

// A process loopback client for `pid`'s process tree, or for everything but it.
ComPtr<IAudioClient> processLoopback(DWORD pid, bool include, std::string& error)
{
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode =
        include ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT activation{};
    activation.vt = VT_BLOB;
    activation.blob.cbSize = sizeof(params);
    activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    ComPtr<Activation> handler;
    handler.Attach(new Activation);
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    HRESULT result = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                                 &activation, handler.Get(), &operation);
    if (FAILED(result) || WaitForSingleObject(handler->done, kActivateTimeoutMs) != WAIT_OBJECT_0) {
        error = "process loopback is not available (" + std::to_string(result) + ")";
        return nullptr;
    }
    ComPtr<IUnknown> unknown;
    HRESULT activated = E_FAIL;
    ComPtr<IAudioClient> client;
    if (FAILED(operation->GetActivateResult(&activated, &unknown)) || FAILED(activated) || FAILED(unknown.As(&client))) {
        error = "cannot activate process loopback (" + std::to_string(activated) + ")";
        return nullptr;
    }
    return client;
}

bool initialize(Session& s, const WAVEFORMATEX* format, DWORD flags, std::string& error)
{
    flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (HRESULT e = s.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0, format, nullptr);
        FAILED(e)) {
        error = "cannot open the stream (" + std::to_string(e) + ")";
        return false;
    }
    s.sampleRate = double(format->nSamplesPerSec);
    s.channels = format->nChannels;
    return true;
}

bool openDevice(Session& s, const std::vector<int>& channels, std::string& error)
{
    const ComPtr<IMMDevice> device = endpoint(s.target.device);
    s.client = activate(device.Get());
    const MixFormat mix = s.client ? floatMix(s.client.Get()) : MixFormat{};
    if (mix.bytes.empty()) {
        error = "cannot open the device";
        return false;
    }
    if (!initialize(s, mix.get(), 0, error))
        return false;
    for (int channel : channels)
        if (channel >= 1 && size_t(channel) <= s.channels && s.tapCount < s.taps.size())
            s.taps[s.tapCount++] = size_t(channel - 1);
    if (s.tapCount == 0)
        for (size_t c = 0; c < s.channels && c < s.taps.size(); ++c)
            s.taps[s.tapCount++] = c;
    s.uid = s.target.device;
    s.name = friendlyName(device.Get());
    return true;
}

// The default output's own loopback: what system audio falls back to without process loopback.
bool openOutputLoopback(Session& s, std::string& error)
{
    s.client = activate(endpoint(defaultEndpoint(eRender)).Get());
    const MixFormat mix = s.client ? floatMix(s.client.Get()) : MixFormat{};
    if (mix.bytes.empty()) {
        error = "no output device to loop back";
        return false;
    }
    return initialize(s, mix.get(), AUDCLNT_STREAMFLAGS_LOOPBACK, error);
}

bool openTap(Session& s, std::string& error)
{
    const bool app = s.target.kind == Target::Kind::App;
    const WAVEFORMATEX format = tapFormat();
    s.client = processLoopback(app ? s.target.pid : GetCurrentProcessId(), app, error);
    const bool opened = s.client && initialize(s, &format, AUDCLNT_STREAMFLAGS_LOOPBACK, error);
    if (!opened && (app || !openOutputLoopback(s, error)))
        return false;
    for (size_t c = 0; c < s.channels && c < s.taps.size(); ++c)
        s.taps[s.tapCount++] = c;
    s.uid = app ? "app" : "system";
    s.name = "system audio";
    if (app)
        for (const Process& p : processes())
            if (p.pid == s.target.pid)
                s.name = "app audio (" + appInfo(p, false).name + ")";
    return true;
}

std::unique_ptr<Session> start(const Target& target, const std::vector<int>& channels, AudioAnalysis& analysis,
                               std::string& error)
{
    auto s = std::make_unique<Session>();
    s->analysis = &analysis;
    s->target = target;
    if (!(target.tap() ? openTap(*s, error) : openDevice(*s, channels, error)))
        return nullptr;
    if (s->tapCount == 0 || s->sampleRate <= 0.0) {
        error = "no input channels";
        return nullptr;
    }
    s->scale = 1.0f / float(s->tapCount);
    if (FAILED(s->client->GetService(IID_PPV_ARGS(&s->capture))) || FAILED(s->client->SetEventHandle(s->event))) {
        error = "cannot capture from the stream";
        return nullptr;
    }
    if (HRESULT e = s->client->Start(); FAILED(e)) {
        error = "cannot start the stream (" + std::to_string(e) + ")";
        return nullptr;
    }
    Session* session = s.get();
    s->thread = std::thread([session] { run(*session); });
    return s;
}

// Endpoint changes arrive on a system thread; `changed` only queues work, and gets whether the device list changed.
class Notifications final : public IMMNotificationClient {
public:
    explicit Notifications(std::function<void(bool list)> changed) : changed_(std::move(changed)) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --references_;
        if (left == 0)
            delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override
    {
        if (id != __uuidof(IUnknown) && id != __uuidof(IMMNotificationClient)) {
            *object = nullptr;
            return E_NOINTERFACE;
        }
        *object = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return notify(true); }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return notify(true); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return notify(true); }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return notify(false); }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    HRESULT notify(bool list)
    {
        changed_(list);
        return S_OK;
    }

    std::function<void(bool)> changed_;
    std::atomic<ULONG> references_{1};
};

bool sameStatus(const AudioInputStatus& a, const AudioInputStatus& b)
{
    return a.requested == b.requested && a.channels == b.channels && a.uid == b.uid && a.name == b.name &&
           a.sampleRate == b.sampleRate && a.state == b.state && a.permission == b.permission &&
           a.restarts == b.restarts;
}

} // namespace

struct AudioInput::Impl : std::enable_shared_from_this<AudioInput::Impl> {
    explicit Impl(AudioAnalysis& analysis) : analysis(analysis) {}

    void post(std::function<void()> task);
    void work();
    void reconcile(const std::string& why, bool force = false);
    void publish(AudioInputStatus next, const std::string& event);
    void tick();
    void reportPermission(bool request);
    void watchDevices();
    void deviceChanged(bool list);
    AudioInputStatus current() const
    {
        std::lock_guard lock(mutex);
        return status;
    }

    AudioAnalysis& analysis;
    std::mutex changedMutex;
    Changed changed;
    std::function<void()> devicesChanged;

    std::mutex queueMutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> tasks;
    bool quit = false;
    std::thread worker;

    // Queue only.
    bool askedPermission = false;
    std::string requested;
    std::vector<int> channels;
    std::unique_ptr<Session> session;
    Target failed;
    Clock::time_point retryAt;
    uint64_t lastCallbacks = 0;
    int stalled = 0;
    ComPtr<IMMDeviceEnumerator> devices;
    ComPtr<IMMNotificationClient> notifications;

    mutable std::mutex mutex;
    AudioInputStatus status;
};

void AudioInput::Impl::post(std::function<void()> task)
{
    {
        std::lock_guard lock(queueMutex);
        if (quit)
            return;
        tasks.push_back(std::move(task));
    }
    wake.notify_one();
}

// The input's serial queue: tasks in order, and the watchdog once a second.
void AudioInput::Impl::work()
{
    ComScope com;
    auto nextTick = Clock::now() + std::chrono::seconds(1);
    std::unique_lock lock(queueMutex);
    for (;;) {
        wake.wait_until(lock, nextTick, [this] { return quit || !tasks.empty(); });
        if (!tasks.empty()) {
            std::function<void()> task = std::move(tasks.front());
            tasks.pop_front();
            lock.unlock();
            task();
            lock.lock();
        } else if (quit) {
            break;
        } else if (Clock::now() >= nextTick) {
            nextTick += std::chrono::seconds(1);
            lock.unlock();
            tick();
            lock.lock();
        }
    }
    lock.unlock();
    if (devices && notifications)
        devices->UnregisterEndpointNotificationCallback(notifications.Get());
    session.reset();
    devices.Reset();
}

void AudioInput::Impl::watchDevices()
{
    devices = enumerator();
    // The input may be gone by the time a notification arrives.
    notifications.Attach(new Notifications([weak = weak_from_this()](bool list) {
        if (const std::shared_ptr<Impl> impl = weak.lock())
            impl->post([raw = impl.get(), list] { raw->deviceChanged(list); });
    }));
    if (devices)
        devices->RegisterEndpointNotificationCallback(notifications.Get());
}

void AudioInput::Impl::deviceChanged(bool list)
{
    if (list) {
        std::lock_guard lock(changedMutex);
        if (devicesChanged)
            devicesChanged();
    }
    reconcile("device change");
}

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

void AudioInput::Impl::reportPermission(bool request)
{
    AudioInputStatus next = current();
    next.permission = AudioInput::permission();
    publish(next, "microphone permission: " + next.permission);
    askedPermission = request;
}

void AudioInput::Impl::reconcile(const std::string& why, bool force)
{
    if (!askedPermission && (requested != "off" || current().permission.empty()))
        reportPermission(requested != "off");
    AudioInputStatus next = current();
    next.requested = requested;
    next.channels = channels;
    if (requested == "off") {
        const bool wasRunning = session != nullptr;
        session.reset();
        next.uid = next.name = {};
        next.sampleRate = 0.0;
        next.state = "off";
        return publish(next, wasRunning || why == "select" ? why + ": input off" : "");
    }

    Target target = resolve(requested);
    bool fallback = false;
    const bool backingOff = target.valid() && target == failed && Clock::now() < retryAt;
    if ((!target.valid() && !requested.empty()) || backingOff) {
        target = Target::input(defaultEndpoint(eCapture));
        fallback = true;
    }
    const bool healthy = session && session->target == target && !force && !session->failed;
    if (healthy) {
        next.state = fallback ? "fallback" : "running";
        return publish(next, "");
    }

    const bool restart = session && session->target == target;
    session.reset();
    std::string error;
    if (target.valid()) {
        session = start(target, channels, analysis, error);
        if (!session) {
            failed = target;
            retryAt = Clock::now() + kRetryAfterFailure;
        }
    }
    const Target fallbackTarget = Target::input(defaultEndpoint(eCapture));
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
        if (stalled >= kStalledTicks || session->failed)
            return reconcile("callbacks stopped", true);
    }
    reconcile("device change");
}

AudioInput::AudioInput(AudioAnalysis& analysis) : impl_(std::make_shared<Impl>(analysis))
{
    Impl* impl = impl_.get();
    if (disabled())
        impl->requested = "off";
    impl->worker = std::thread([impl] { impl->work(); });
    impl->post([impl] { impl->watchDevices(); });
}

AudioInput::~AudioInput()
{
    Impl* impl = impl_.get();
    {
        std::lock_guard lock(impl->queueMutex);
        impl->quit = true;
    }
    impl->wake.notify_one();
    impl->worker.join();
}

std::vector<AudioDeviceInfo> AudioInput::devices()
{
    ComScope com;
    const std::string fallback = defaultEndpoint(eCapture);
    std::vector<AudioDeviceInfo> result;
    for (const ComPtr<IMMDevice>& device : endpoints(eCapture)) {
        const ComPtr<IAudioClient> client = activate(device.Get());
        WAVEFORMATEX* mix = nullptr;
        if (!client || FAILED(client->GetMixFormat(&mix)))
            continue;
        std::string id = endpointId(device.Get());
        result.push_back({id, friendlyName(device.Get()), int(mix->nChannels), double(mix->nSamplesPerSec), id == fallback});
        CoTaskMemFree(mix);
    }
    return result;
}

std::vector<AudioAppInfo> AudioInput::apps()
{
    ComScope com;
    std::map<DWORD, bool> playing;
    for (const ComPtr<IMMDevice>& device : endpoints(eRender)) {
        ComPtr<IAudioSessionManager2> manager;
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager)) ||
            FAILED(manager->GetSessionEnumerator(&sessions)))
            continue;
        int count = 0;
        sessions->GetCount(&count);
        for (int i = 0; i < count; ++i) {
            ComPtr<IAudioSessionControl> control;
            ComPtr<IAudioSessionControl2> control2;
            DWORD pid = 0;
            AudioSessionState state = AudioSessionStateInactive;
            if (FAILED(sessions->GetSession(i, &control)) || FAILED(control.As(&control2)) ||
                FAILED(control2->GetProcessId(&pid)) || pid == 0 || pid == GetCurrentProcessId())
                continue;
            control->GetState(&state);
            playing[pid] = playing[pid] || state == AudioSessionStateActive;
        }
    }
    std::vector<AudioAppInfo> result;
    for (const Process& p : processes())
        if (const auto it = playing.find(p.pid); it != playing.end())
            result.push_back(appInfo(p, it->second));
    return result;
}

std::string AudioInput::permission()
{
    return permissionName();
}

void AudioInput::select(std::string device, std::vector<int> channels)
{
    if (disabled())
        device = "off";
    Impl* impl = impl_.get();
    impl->post([impl, device = std::move(device), channels = std::move(channels)] {
        impl->requested = device;
        impl->channels = channels;
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

void becomeResponsibleProcess(char**) {}

} // namespace azl
