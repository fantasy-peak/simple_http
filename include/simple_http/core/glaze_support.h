#pragma once

// Feature gate for the glaze-backed helpers: JSON reflection (proto/json.h,
// proto/params.h), the typed extractors built on them (handler/extract.h) and
// the OpenAPI document model.
//
// glaze 7.x needs C++23: it uses concepts and std::byteswap, so it cannot be
// included from a translation unit compiled as C++17. The ported tree therefore
// keeps those headers out of a real C++17 build, and enables them automatically
// for a downstream translation unit that compiles this same include root as
// C++23 or newer:
//
//   g++ -std=c++23 -I cpp17/include ...        // helpers enabled
//   g++ -std=c++17 -I cpp17/include ...        // helpers disabled
//
// Override with -DSIMPLE_HTTP_CPP17_ENABLE_GLAZE=0 to opt out, or =1 to force
// enablement. Forcing it on for a compiler that reports an old __cplusplus (or
// for a build without glaze on the include path) is the caller's decision and
// fails at the #include below.
//
// The macro is evaluated per translation unit. Mixing standards across TUs is
// fine because the gated APIs are header-only templates and nothing in the
// ungated surface depends on them.

#if !defined(SIMPLE_HTTP_CPP17_ENABLE_GLAZE)
#if defined(__cplusplus) && __cplusplus > 202002L && defined(__has_include)
#if __has_include(<glaze/glaze.hpp>)
#define SIMPLE_HTTP_CPP17_ENABLE_GLAZE 1
#else
#define SIMPLE_HTTP_CPP17_ENABLE_GLAZE 0
#endif
#else
#define SIMPLE_HTTP_CPP17_ENABLE_GLAZE 0
#endif
#endif

#if SIMPLE_HTTP_CPP17_ENABLE_GLAZE
#include <glaze/glaze.hpp>
#endif
