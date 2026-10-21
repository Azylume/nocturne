#include "sync/discovery.hpp"

#import <Foundation/Foundation.h>

#include <arpa/inet.h>
#include <dispatch/dispatch.h>
#include <dns_sd.h>

#include <mutex>

namespace azl {

namespace {

std::vector<uint8_t> txtRecord(const std::map<std::string, std::string>& txt)
{
    TXTRecordRef record;
    TXTRecordCreate(&record, 0, nullptr);
    for (const auto& [key, value] : txt)
        TXTRecordSetValue(&record, key.c_str(), uint8_t(value.size()), value.data());
    const auto* bytes = static_cast<const uint8_t*>(TXTRecordGetBytesPtr(&record));
    std::vector<uint8_t> out(bytes, bytes + TXTRecordGetLength(&record));
    TXTRecordDeallocate(&record);
    return out;
}

std::map<std::string, std::string> parseTxt(uint16_t length, const unsigned char* bytes)
{
    std::map<std::string, std::string> txt;
    const uint16_t count = TXTRecordGetCount(length, bytes);
    for (uint16_t i = 0; i < count; ++i) {
        char key[256];
        uint8_t size = 0;
        const void* value = nullptr;
        if (TXTRecordGetItemAtIndex(length, bytes, i, sizeof key, key, &size, &value) == kDNSServiceErr_NoError)
            txt[key] = value ? std::string(static_cast<const char*>(value), size) : std::string();
    }
    return txt;
}

void report(const char* what, DNSServiceErrorType error)
{
    std::printf("bonjour: %s failed (%d)%s\n", what, int(error),
                error == kDNSServiceErr_PolicyDenied ? ": Local Network access denied for this app" : "");
    std::fflush(stdout);
}

std::string withoutTrailingDot(std::string host)
{
    if (!host.empty() && host.back() == '.')
        host.pop_back();
    return host;
}

} // namespace

// Re-registering on every change makes browsers drop the service and resolve it afresh, which they do not reliably do
// for a TXT update alone.
struct Advertiser::Impl {
    dispatch_queue_t queue = dispatch_queue_create("nocturne.advertiser", DISPATCH_QUEUE_SERIAL);
    std::string name;
    uint16_t port = 0;
    DNSServiceRef ref = nullptr;

    // On the queue.
    void announce(const std::map<std::string, std::string>& txt)
    {
        if (ref)
            DNSServiceRefDeallocate(ref);
        const std::vector<uint8_t> record = txtRecord(txt);
        const DNSServiceErrorType error =
            DNSServiceRegister(&ref, 0, 0, name.c_str(), kServiceType, nullptr, nullptr, htons(port),
                               uint16_t(record.size()), record.data(), &Impl::registered, this);
        if (error == kDNSServiceErr_NoError)
            DNSServiceSetDispatchQueue(ref, queue);
        else
            ref = nullptr;
        if (error != kDNSServiceErr_NoError)
            report("advertising", error);
    }

    static void DNSSD_API registered(DNSServiceRef, DNSServiceFlags, DNSServiceErrorType error, const char* name,
                                     const char*, const char*, void*)
    {
        if (error != kDNSServiceErr_NoError)
            report("advertising", error);
        else
            std::printf("bonjour: advertising \"%s\"\n", name);
        std::fflush(stdout);
    }
};

Advertiser::Advertiser(const std::string& name, uint16_t port, std::map<std::string, std::string> txt)
    : impl_(std::make_shared<Impl>())
{
    Impl* impl = impl_.get();
    impl->name = name;
    impl->port = port;
    dispatch_sync(impl->queue, ^{ impl->announce(txt); });
}

Advertiser::~Advertiser()
{
    Impl* impl = impl_.get();
    dispatch_sync(impl->queue, ^{
        if (impl->ref)
            DNSServiceRefDeallocate(impl->ref);
    });
}

void Advertiser::update(std::map<std::string, std::string> txt)
{
    Impl* impl = impl_.get();
    dispatch_sync(impl->queue, ^{ impl->announce(txt); });
}

// Callbacks run on `queue`, where every ref is also released, so none runs after the destructor. A resolve stays
// open while its service is there, so TXT changes (a node promoted to main) come through.
struct Browser::Impl {
    struct Resolver {
        Impl* impl;
        std::string name;
        DNSServiceRef ref = nullptr;
    };
    struct Found {
        Service service;
        std::map<uint32_t, Resolver*> resolvers;
    };

    dispatch_queue_t queue = dispatch_queue_create("nocturne.browser", DISPATCH_QUEUE_SERIAL);
    DNSServiceRef browse = nullptr;
    OnChange onChange;
    mutable std::mutex mutex;
    std::map<std::string, Found> found;

    std::vector<Service> snapshot() const
    {
        std::lock_guard lock(mutex);
        std::vector<Service> services;
        for (const auto& [name, f] : found)
            if (f.service.port)
                services.push_back(f.service);
        return services;
    }

    void changed()
    {
        if (onChange)
            onChange(snapshot());
    }

    static void release(Resolver* r)
    {
        DNSServiceRefDeallocate(r->ref);
        delete r;
    }

    void removed(const std::string& name, uint32_t interface)
    {
        {
            std::lock_guard lock(mutex);
            auto it = found.find(name);
            if (it == found.end())
                return;
            if (auto r = it->second.resolvers.find(interface); r != it->second.resolvers.end()) {
                release(r->second);
                it->second.resolvers.erase(r);
            }
            if (!it->second.resolvers.empty())
                return;
            found.erase(it);
        }
        changed();
    }

    void added(const std::string& name, uint32_t interface, const char* type, const char* domain)
    {
        std::lock_guard lock(mutex);
        Found& f = found[name];
        f.service.name = name;
        if (f.resolvers.count(interface))
            return;
        auto* r = new Resolver{this, name};
        if (DNSServiceResolve(&r->ref, 0, interface, name.c_str(), type, domain, &Impl::resolved, r) !=
            kDNSServiceErr_NoError) {
            delete r;
            return;
        }
        DNSServiceSetDispatchQueue(r->ref, queue);
        f.resolvers[interface] = r;
    }

    static void DNSSD_API resolved(DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType error, const char*,
                                   const char* host, uint16_t port, uint16_t txtLength, const unsigned char* txt,
                                   void* context)
    {
        auto* r = static_cast<Resolver*>(context);
        if (error != kDNSServiceErr_NoError)
            return;
        Impl* impl = r->impl;
        {
            std::lock_guard lock(impl->mutex);
            auto it = impl->found.find(r->name);
            if (it == impl->found.end())
                return;
            it->second.service = {r->name, withoutTrailingDot(host), ntohs(port), parseTxt(txtLength, txt)};
        }
        impl->changed();
    }

    static void DNSSD_API browsed(DNSServiceRef, DNSServiceFlags flags, uint32_t interface, DNSServiceErrorType error,
                                  const char* name, const char* type, const char* domain, void* context)
    {
        if (error != kDNSServiceErr_NoError)
            return report("browsing", error);
        auto* impl = static_cast<Impl*>(context);
        if (flags & kDNSServiceFlagsAdd)
            impl->added(name, interface, type, domain);
        else
            impl->removed(name, interface);
    }
};

Browser::Browser(OnChange onChange) : impl_(std::make_shared<Impl>())
{
    Impl* impl = impl_.get();
    impl->onChange = std::move(onChange);
    dispatch_sync(impl->queue, ^{
        if (DNSServiceBrowse(&impl->browse, 0, 0, kServiceType, nullptr, &Impl::browsed, impl) == kDNSServiceErr_NoError)
            DNSServiceSetDispatchQueue(impl->browse, impl->queue);
        else
            impl->browse = nullptr;
        if (!impl->browse)
            report("browsing", kDNSServiceErr_Unknown);
    });
}

Browser::~Browser()
{
    Impl* impl = impl_.get();
    dispatch_sync(impl->queue, ^{
        if (impl->browse)
            DNSServiceRefDeallocate(impl->browse);
        std::lock_guard lock(impl->mutex);
        for (auto& [name, f] : impl->found)
            for (auto& [interface, r] : f.resolvers)
                Impl::release(r);
        impl->found.clear();
    });
}

std::vector<Service> Browser::services() const
{
    return impl_->snapshot();
}

std::string machineName()
{
    @autoreleasepool {
        NSString* name = NSHost.currentHost.localizedName ?: NSProcessInfo.processInfo.hostName;
        return name ? std::string(name.UTF8String) : "Nocturne";
    }
}

} // namespace azl
