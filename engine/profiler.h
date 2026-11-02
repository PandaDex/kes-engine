#pragma once
#ifndef KES_PROFILING
#define KES_PROFILING 0
#endif

#if KES_PROFILING
#include <tracy/Tracy.hpp>
#endif

#if KES_PROFILING
#ifndef KES_PROFILE_CORE
#define KES_PROFILE_CORE 1
#endif
#ifndef KES_PROFILE_INPUT
#define KES_PROFILE_INPUT 1
#endif
#ifndef KES_PROFILE_WINDOW
#define KES_PROFILE_WINDOW 1
#endif
#ifndef KES_PROFILE_RENDER
#define KES_PROFILE_RENDER 1
#endif
#ifndef KES_PROFILE_GPU
#define KES_PROFILE_GPU 1
#endif
#ifndef KES_PROFILE_WORLD
#define KES_PROFILE_WORLD 1
#endif
#ifndef KES_PROFILE_ENTITIES
#define KES_PROFILE_ENTITIES 1
#endif
#ifndef KES_PROFILE_PHYSICS
#define KES_PROFILE_PHYSICS 1
#endif
#ifndef KES_PROFILE_ASSETS
#define KES_PROFILE_ASSETS 1
#endif
#ifndef KES_PROFILE_AUDIO
#define KES_PROFILE_AUDIO 1
#endif
#ifndef KES_PROFILE_IO
#define KES_PROFILE_IO 1
#endif
#else
#undef KES_PROFILE_CORE
#undef KES_PROFILE_INPUT
#undef KES_PROFILE_WINDOW
#undef KES_PROFILE_RENDER
#undef KES_PROFILE_GPU
#undef KES_PROFILE_WORLD
#undef KES_PROFILE_ENTITIES
#undef KES_PROFILE_PHYSICS
#undef KES_PROFILE_ASSETS
#undef KES_PROFILE_AUDIO
#undef KES_PROFILE_IO
#define KES_PROFILE_CORE 0
#define KES_PROFILE_INPUT 0
#define KES_PROFILE_WINDOW 0
#define KES_PROFILE_RENDER 0
#define KES_PROFILE_GPU 0
#define KES_PROFILE_WORLD 0
#define KES_PROFILE_ENTITIES 0
#define KES_PROFILE_PHYSICS 0
#define KES_PROFILE_ASSETS 0
#define KES_PROFILE_AUDIO 0
#define KES_PROFILE_IO 0
#endif

#define KES_PROFILE_COLOR_CORE     0x9E9E9E
#define KES_PROFILE_COLOR_INPUT    0xFFB300
#define KES_PROFILE_COLOR_WINDOW   0x8D6E63
#define KES_PROFILE_COLOR_RENDER   0x1E88E5
#define KES_PROFILE_COLOR_GPU      0x7B1FA2
#define KES_PROFILE_COLOR_WORLD    0x43A047
#define KES_PROFILE_COLOR_ENTITIES 0x7CB342
#define KES_PROFILE_COLOR_PHYSICS  0xE53935
#define KES_PROFILE_COLOR_ASSETS   0x00ACC1
#define KES_PROFILE_COLOR_AUDIO    0xD81B60
#define KES_PROFILE_COLOR_IO       0x546E7A

#if KES_PROFILING
#define KES__PROF_ZONE_ON(name, color) ZoneScopedNC(name, color)
#define KES__PROF_ZONE_FN_ON(color) ZoneScopedC(color)
#define KES__PROF_ZONE_TEXT_ON(txt, len) ZoneText(txt, len)
#define KES__PROF_ZONE_TEXTF_ON(...) ZoneTextF(__VA_ARGS__)
#define KES__PROF_ZONE_VALUE_ON(value) ZoneValue(static_cast<uint64_t>(value))
#endif
#define KES__PROF_OFF(...) ((void)0)

#define KES__PROF_SELECT_1(op) KES__PROF_##op##_ON
#define KES__PROF_SELECT_0(op) KES__PROF_OFF
#define KES__PROF_SELECT_I(enabled, op) KES__PROF_SELECT_##enabled(op)
#define KES__PROF_SELECT(enabled, op) KES__PROF_SELECT_I(enabled, op)
#define KES__PROF_CAT(cat, op) KES__PROF_SELECT(KES_PROFILE_##cat, op)

#define KES_ZONE(cat, name) KES__PROF_CAT(cat, ZONE)(name, KES_PROFILE_COLOR_##cat)
#define KES_ZONE_FN(cat) KES__PROF_CAT(cat, ZONE_FN)(KES_PROFILE_COLOR_##cat)
#define KES_ZONE_TEXT(cat, txt, len) KES__PROF_CAT(cat, ZONE_TEXT)(txt, len)
#define KES_ZONE_TEXTF(cat, ...) KES__PROF_CAT(cat, ZONE_TEXTF)(__VA_ARGS__)
#define KES_ZONE_VALUE(cat, value) KES__PROF_CAT(cat, ZONE_VALUE)(value)

#if KES_PROFILING
#define KES_PROFILE_FRAME() FrameMark
#define KES_PROFILE_PLOT(name, value) TracyPlot(name, value)
#define KES_PROFILE_MESSAGE(txt, len) TracyMessage(txt, len)
#define KES_PROFILE_MESSAGE_L(literal) TracyMessageL(literal)
#define KES_PROFILE_APP_INFO(txt, len) TracyAppInfo(txt, len)
#define KES_PROFILE_THREAD_NAME(name) tracy::SetThreadName(name)
#define KES_PROFILE_IS_CONNECTED() TracyIsConnected
#else
#define KES_PROFILE_FRAME() ((void)0)
#define KES_PROFILE_PLOT(name, value) ((void)0)
#define KES_PROFILE_MESSAGE(txt, len) ((void)0)
#define KES_PROFILE_MESSAGE_L(literal) ((void)0)
#define KES_PROFILE_APP_INFO(txt, len) ((void)0)
#define KES_PROFILE_THREAD_NAME(name) ((void)0)
#define KES_PROFILE_IS_CONNECTED() false
#endif
