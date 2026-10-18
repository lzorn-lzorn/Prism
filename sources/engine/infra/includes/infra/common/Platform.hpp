
#pragma once

#include <cstddef>
#include <new>
#include <string_view>

#if defined(__cpp_assume) && __cpp_assume >= 202207L
#	define ASSUME(expr) [[assume(expr)]]
#else
#	ifdef _MSC_VER
#		define ASSUME(expr) __assume(expr)
#	elif defined(__clang__) || defined(__GNUC__)
#		if __has_builtin(__builtin_assume)
#			define ASSUME(expr) __builtin_assume(expr)
#		else
#			define ASSUME(expr) do { if (!(expr)) __builtin_unreachable(); } while(0)
#		endif
#	else
#		define ASSUME(expr)
#	endif
#endif

#ifdef __cpp_lib_hardware_interference_size
    inline constexpr size_t CacheLineSize = std::hardware_destructive_interference_size;
#else
    inline constexpr size_t CacheLineSize = 64;
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    // x86/x64 平台，用统一的 CPU_PAUSE
#	if defined(_MSC_VER)
#		include <intrin.h>
#		define CPU_RELAX _mm_pause()
#	else
#		define CPU_RELAX __builtin_ia32_pause()
#	endif
#elif defined(__arm__) || defined(__aarch64__)
#	if defined(_MSC_VER)
#		include <intrin.h>
#	else
#		include <arm_acle.h>
#	endif
#	define CPU_RELAX __yield()
#else
#	define CPU_RELAX std::this_thread::yield()
#endif

#if defined(_MSC_VER)
#   define FORCE_INLINE __forceinline
#   define INTERFACE __declspec(novtable)
#else
#   define FORCE_INLINE inline __attribute__((always_inline))
#   define INTERFACE
#endif

#if defined(__GNUC__) || defined(__clang__)
  #define WARN_IMPL(msg) __builtin_warning(msg)
#elif defined(_MSC_VER)
  #define WARN_IMPL(msg) __pragma(message(__FILE__ "(" STRINGIZE(__LINE__) "): warning: " msg))
#else
  #define WARN_IMPL(msg) \
      [[deprecated(msg)]] static void deprecated_helper() {} \
      deprecated_helper()
#endif

#define STRINGIZE_IMPL(x) #x
#define STRINGIZE(x)  STRINGIZE_IMPL(x)

#define WARNING(msg) do { WARN_IMPL(msg); } while (0)

consteval void warning(std::string_view) {}
struct warning_fn {
    template<size_t N>
    consteval void operator()(const char (&msg)[N]) const {
        WARN_IMPL(msg);
    }
};
inline constexpr warning_fn warn;
