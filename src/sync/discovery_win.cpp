// Discovery on Windows through its own DNS-SD API (windns.h, Windows 10 1809 and later): the same _nocturne._tcp
// services, names and TXT records as Bonjour on macOS (discovery.mm).

#include "sync/discovery.hpp"

#include "net/lan.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windns.h>

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace azl {

namespace {

constexpr auto kDeregisterWait = std::chrono::seconds(2);

std::wstring wide(const std::string& text)
{
    if (text.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size);
    return out;
}

std::string narrow(const wchar_t* text)
{
    if (!text || !*text)
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(size_t(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    out.resize(size_t(size - 1));
    return out;
}

void report(const char* what, DWORD error)
{
    std::printf("bonjour: %s failed (%lu)\n", what, static_cast<unsigned long>(error));
    std::fflush(stdout);
}

const std::string kDomainSuffix = std::string(".") + kServiceType + ".local";

// "Name._nocturne._tcp.local" as "Name", its DNS escapes (\. \\ \DDD) undone; empty when it is not one of ours.
std::string instanceName(const std::string& full)
{
    std::string fullName = full;
    if (!fullName.empty() && fullName.back() == '.')
        fullName.pop_back();
    if (fullName.size() <= kDomainSuffix.size() ||
        fullName.compare(fullName.size() - kDomainSuffix.size(), kDomainSuffix.size(), kDomainSuffix) != 0)
        return {};
    const std::string escaped = fullName.substr(0, fullName.size() - kDomainSuffix.size());
    std::string name;
    for (size_t i = 0; i < escaped.size(); ++i) {
        if (escaped[i] != '\\' || i + 1 == escaped.size()) {
            name += escaped[i];
        } else if (i + 3 < escaped.size() && std::isdigit(static_cast<unsigned char>(escaped[i + 1])) &&
                   std::isdigit(static_cast<unsigned char>(escaped[i + 2])) &&
                   std::isdigit(static_cast<unsigned char>(escaped[i + 3]))) {
            name += char(std::stoi(escaped.substr(i + 1, 3)));
            i += 3;
        } else {
            name += escaped[++i];
        }
    }
    return name;
}

std::wstring fullName(const std::string& name)
{
    std::string escaped;
    for (const char c : name) {
        if (c == '.' || c == '\\')
            escaped += '\\';
        escaped += c;
    }
    return wide(escaped + kDomainSuffix);
}

std::string withoutTrailingDot(std::string host)
{
    if (!host.empty() && host.back() == '.')
        host.pop_back();
    return host;
}

// The objects the API calls back into, found by the number it hands back as context; one destroyed is gone from here,
// so a late callback finds nothing and does nothing.
template <typename T> class Registry {
public:
    void* add(const std::shared_ptr<T>& object)
    {
        std::lock_guard lock(mutex_);
        const uintptr_t id = ++next_;
        objects_[id] = object;
        return reinterpret_cast<void*>(id);
    }
    void remove(void* id)
    {
        std::lock_guard lock(mutex_);
        objects_.erase(reinterpret_cast<uintptr_t>(id));
    }
    std::shared_ptr<T> find(void* id)
    {
        std::lock_guard lock(mutex_);
        const auto found = objects_.find(reinterpret_cast<uintptr_t>(id));
        return found == objects_.end() ? nullptr : found->second.lock();
    }

private:
    std::mutex mutex_;
    uintptr_t next_ = 0;
    std::map<uintptr_t, std::weak_ptr<T>> objects_;
};

} // namespace

// Re-registering on every change makes browsers drop the service and resolve it afresh, as on macOS. A withdrawal waits
// for the system to confirm it, so the next registration of the same name does not collide with it.
struct Advertiser::Impl {
    uint16_t port = 0;
    std::wstring instance;
    std::wstring host;
    std::mutex mutex;
    std::condition_variable settled;
    PDNS_SERVICE_INSTANCE registered = nullptr;
    DNS_SERVICE_CANCEL cancel{};
    bool withdrawing = false;
    void* context = nullptr;

    static Registry<Impl>& registry()
    {
        static Registry<Impl> instances;
        return instances;
    }

    static void WINAPI onRegistered(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance)
    {
        if (status != ERROR_SUCCESS || !instance) {
            report("advertising", status);
        } else {
            std::printf("bonjour: advertising \"%s\"\n", instanceName(narrow(instance->pszInstanceName)).c_str());
            std::fflush(stdout);
        }
        if (instance)
            DnsServiceFreeInstance(instance);
    }

    static void WINAPI onDeregistered(DWORD, PVOID context, PDNS_SERVICE_INSTANCE instance)
    {
        if (instance)
            DnsServiceFreeInstance(instance);
        if (auto self = registry().find(context)) {
            std::lock_guard lock(self->mutex);
            self->withdrawing = false;
            self->settled.notify_all();
        }
    }

    DNS_SERVICE_REGISTER_REQUEST request(PDNS_SERVICE_REGISTER_COMPLETE callback)
    {
        DNS_SERVICE_REGISTER_REQUEST r{};
        r.Version = DNS_QUERY_REQUEST_VERSION1;
        r.InterfaceIndex = 0;
        r.pServiceInstance = registered;
        r.pRegisterCompletionCallback = callback;
        r.pQueryContext = context;
        r.unicastEnabled = FALSE;
        return r;
    }

    // Under `mutex`, which it releases while it waits.
    void withdraw(std::unique_lock<std::mutex>& lock)
    {
        if (!registered)
            return;
        DNS_SERVICE_REGISTER_REQUEST r = request(&Impl::onDeregistered);
        withdrawing = DnsServiceDeRegister(&r, nullptr) == DNS_REQUEST_PENDING;
        settled.wait_for(lock, kDeregisterWait, [&] { return !withdrawing; });
        DnsServiceFreeInstance(registered);
        registered = nullptr;
    }

    void announce(const std::map<std::string, std::string>& txt)
    {
        std::unique_lock lock(mutex);
        withdraw(lock);
        std::vector<std::wstring> keys, values;
        for (const auto& [key, value] : txt) {
            keys.push_back(wide(key));
            values.push_back(wide(value));
        }
        std::vector<PCWSTR> keyPointers, valuePointers;
        for (size_t i = 0; i < keys.size(); ++i) {
            keyPointers.push_back(keys[i].c_str());
            valuePointers.push_back(values[i].c_str());
        }
        registered = DnsServiceConstructInstance(instance.c_str(), host.c_str(), nullptr, nullptr, port, 0, 0,
                                                 DWORD(keys.size()), keyPointers.data(), valuePointers.data());
        if (!registered)
            return report("advertising", GetLastError());
        DNS_SERVICE_REGISTER_REQUEST r = request(&Impl::onRegistered);
        if (const DWORD status = DnsServiceRegister(&r, &cancel); status != DNS_REQUEST_PENDING)
            report("advertising", status);
    }
};

Advertiser::Advertiser(const std::string& name, uint16_t port, std::map<std::string, std::string> txt)
    : impl_(std::make_shared<Impl>())
{
    impl_->port = port;
    impl_->instance = fullName(name);
    impl_->host = wide(bonjourName());
    impl_->context = Impl::registry().add(impl_);
    impl_->announce(txt);
}

Advertiser::~Advertiser()
{
    {
        std::unique_lock lock(impl_->mutex);
        impl_->withdraw(lock);
    }
    Impl::registry().remove(impl_->context);
}

void Advertiser::update(std::map<std::string, std::string> txt)
{
    impl_->announce(txt);
}

// Browse and resolve callbacks run on the system's threads. Each instance the browse reports is resolved once per
// report, so a re-registered service (a node promoted to main) comes through with its new TXT. onChange runs under
// `calling`, which the destructor takes too, so none runs after it.
struct Browser::Impl {
    OnChange onChange;
    void* context = nullptr;
    mutable std::mutex mutex;
    DNS_SERVICE_CANCEL browse{};
    bool browsing = false;
    std::map<std::string, Service> found;
    std::map<std::string, std::unique_ptr<DNS_SERVICE_CANCEL>> resolving;
    std::mutex calling;
    bool stopped = false;

    static Registry<Impl>& registry()
    {
        static Registry<Impl> browsers;
        return browsers;
    }

    // Under `mutex`.
    std::vector<Service> snapshot() const
    {
        std::vector<Service> services;
        for (const auto& [name, service] : found)
            if (service.port)
                services.push_back(service);
        return services;
    }

    void changed(std::vector<Service> services)
    {
        std::lock_guard lock(calling);
        if (!stopped && onChange)
            onChange(services);
    }

    // A resolve's context: its browser's and the instance's name.
    struct Resolve {
        void* browser;
        std::string name;
    };

    // Under `mutex`.
    void resolve(const std::string& name)
    {
        if (resolving.contains(name))
            return;
        const std::wstring query = fullName(name);
        auto cancel = std::make_unique<DNS_SERVICE_CANCEL>();
        DNS_SERVICE_RESOLVE_REQUEST r{};
        r.Version = DNS_QUERY_REQUEST_VERSION1;
        r.InterfaceIndex = 0;
        r.QueryName = const_cast<PWSTR>(query.c_str());
        r.pResolveCompletionCallback = &Impl::onResolved;
        r.pQueryContext = new Resolve{context, name};
        const DWORD status = DnsServiceResolve(&r, cancel.get());
        if (status != DNS_REQUEST_PENDING) {
            delete static_cast<Resolve*>(r.pQueryContext);
            return report("resolving", status);
        }
        resolving[name] = std::move(cancel);
    }

    static void WINAPI onResolved(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance)
    {
        const std::unique_ptr<Resolve> resolve(static_cast<Resolve*>(context));
        std::optional<Service> service;
        if (status == ERROR_SUCCESS && instance) {
            service = Service{resolve->name, withoutTrailingDot(narrow(instance->pszHostName)), instance->wPort, {}};
            for (DWORD i = 0; i < instance->dwPropertyCount; ++i)
                service->txt[narrow(instance->keys[i])] = narrow(instance->values[i]);
        }
        if (instance)
            DnsServiceFreeInstance(instance);
        const std::shared_ptr<Impl> self = registry().find(resolve->browser);
        if (!self)
            return;
        std::vector<Service> services;
        {
            std::lock_guard lock(self->mutex);
            self->resolving.erase(resolve->name);
            const auto at = self->found.find(resolve->name);
            if (!service || at == self->found.end() || at->second == *service)
                return;
            at->second = *service;
            services = self->snapshot();
        }
        self->changed(std::move(services));
    }

    static void WINAPI onBrowsed(DWORD status, PVOID context, PDNS_RECORD records)
    {
        if (status != ERROR_SUCCESS && status != ERROR_CANCELLED)
            report("browsing", status);
        const std::shared_ptr<Impl> self = status == ERROR_SUCCESS ? registry().find(context) : nullptr;
        bool removed = false;
        std::vector<Service> services;
        if (self) {
            std::lock_guard lock(self->mutex);
            for (PDNS_RECORD r = records; r; r = r->pNext) {
                const std::string name = r->wType == DNS_TYPE_PTR ? instanceName(narrow(r->Data.PTR.pNameHost)) : "";
                if (name.empty())
                    continue;
                if (r->dwTtl == 0) {
                    removed = self->found.erase(name) > 0 || removed;
                    continue;
                }
                self->found.try_emplace(name, Service{name, {}, 0, {}});
                self->resolve(name);
            }
            services = self->snapshot();
        }
        if (records)
            DnsRecordListFree(records, DnsFreeRecordList);
        if (removed)
            self->changed(std::move(services));
    }
};

Browser::Browser(OnChange onChange) : impl_(std::make_shared<Impl>())
{
    impl_->onChange = std::move(onChange);
    impl_->context = Impl::registry().add(impl_);
    static const std::wstring query = wide(std::string(kServiceType) + ".local");
    DNS_SERVICE_BROWSE_REQUEST r{};
    r.Version = DNS_QUERY_REQUEST_VERSION1;
    r.InterfaceIndex = 0;
    r.QueryName = query.c_str();
    r.pBrowseCallback = &Impl::onBrowsed;
    r.pQueryContext = impl_->context;
    std::lock_guard lock(impl_->mutex);
    const DWORD status = DnsServiceBrowse(&r, &impl_->browse);
    impl_->browsing = status == DNS_REQUEST_PENDING;
    if (!impl_->browsing)
        report("browsing", status);
}

Browser::~Browser()
{
    {
        std::lock_guard lock(impl_->calling);
        impl_->stopped = true;
    }
    Impl::registry().remove(impl_->context);
    std::lock_guard lock(impl_->mutex);
    if (impl_->browsing)
        DnsServiceBrowseCancel(&impl_->browse);
    for (auto& [name, cancel] : impl_->resolving)
        DnsServiceResolveCancel(cancel.get());
    impl_->resolving.clear();
}

std::vector<Service> Browser::services() const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->snapshot();
}

std::string machineName()
{
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(name, &size))
        return "Nocturne";
    return narrow(name);
}

} // namespace azl
