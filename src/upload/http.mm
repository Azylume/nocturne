#include "upload/http.hpp"

#import <Foundation/Foundation.h>

#include <cctype>

namespace azl {

namespace {

NSURLSession* session()
{
    static NSURLSession* shared = [] {
        NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        config.timeoutIntervalForRequest = 120;
        config.HTTPShouldSetCookies = NO;
        config.URLCache = nil;
        return [NSURLSession sessionWithConfiguration:config];
    }();
    return shared;
}

std::string lowercase(std::string text)
{
    for (char& c : text)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

} // namespace

HttpResponse httpRequest(const HttpRequest& request)
{
    @autoreleasepool {
        NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:request.url.c_str()]];
        if (!url)
            throw TransportError("bad URL " + request.url);
        NSMutableURLRequest* r = [NSMutableURLRequest requestWithURL:url];
        r.HTTPMethod = [NSString stringWithUTF8String:request.method.c_str()];
        for (const auto& [name, value] : request.headers)
            if (name != "host")
                [r setValue:[NSString stringWithUTF8String:value.c_str()]
                    forHTTPHeaderField:[NSString stringWithUTF8String:name.c_str()]];
        if (!request.body.empty())
            r.HTTPBody = [NSData dataWithBytes:request.body.data() length:request.body.size()];

        __block HttpResponse response;
        __block std::string failure;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        NSURLSessionDataTask* task =
            [session() dataTaskWithRequest:r
                         completionHandler:^(NSData* data, NSURLResponse* reply, NSError* error) {
                           if (error) {
                               failure = error.localizedDescription.UTF8String;
                           } else {
                               NSHTTPURLResponse* http = (NSHTTPURLResponse*)reply;
                               response.status = int(http.statusCode);
                               for (NSString* name in http.allHeaderFields)
                                   response.headers[lowercase(name.UTF8String)] =
                                       [http.allHeaderFields[name] UTF8String];
                               if (data)
                                   response.body.assign(static_cast<const char*>(data.bytes), data.length);
                           }
                           dispatch_semaphore_signal(done);
                         }];
        [task resume];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        if (!failure.empty())
            throw TransportError(request.method + " " + request.url + ": " + failure);
        return response;
    }
}

} // namespace azl
