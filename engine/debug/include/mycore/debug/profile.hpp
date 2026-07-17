#pragma once

#include <tracy/Tracy.hpp>

#define MYCORE_PROFILE_ZONE(name) ZoneScopedN(name)
#define MYCORE_PROFILE_FRAME() FrameMark
#define MYCORE_PROFILE_THREAD(name) tracy::SetThreadName(name)
