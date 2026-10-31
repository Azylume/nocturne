#pragma once

#include "platform/thread_priority.hpp"

#include <pthread/qos.h>

namespace azl {

inline qos_class_t qosClass(ThreadRole role)
{
    switch (role) {
    case ThreadRole::Frames: return QOS_CLASS_USER_INTERACTIVE;
    case ThreadRole::Decode: return QOS_CLASS_USER_INITIATED;
    case ThreadRole::Background: return QOS_CLASS_UTILITY;
    case ThreadRole::Idle: return QOS_CLASS_BACKGROUND;
    }
    return QOS_CLASS_DEFAULT;
}

} // namespace azl
