#pragma once

#include <cstdlib>
#include <optional>
#include <string>

inline void setEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    value ? setenv(name, value, 1) : unsetenv(name);
#endif
}

// Sets a variable for one scope, then puts back what was there.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name)
    {
        if (const char* old = std::getenv(name))
            old_ = old;
        setEnv(name, value);
    }
    ~ScopedEnv() { setEnv(name_.c_str(), old_ ? old_->c_str() : nullptr); }

private:
    std::string name_;
    std::optional<std::string> old_;
};
