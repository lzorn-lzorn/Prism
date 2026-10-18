
#pragma once

#ifndef NDEBUG
#	ifdef _MSC_VER
#		include <intrin.h>
#		define BREAKPOINT() __debugbreak()
#	elif defined(__clang__) || defined(__GNUC__)
#		if defined(__has_builtin) && __has_builtin(__builtin_debugtrap)
#			define BREAKPOINT() __builtin_debugtrap()
#		else
#			include <csignal>
#			define BREAKPOINT() raise(SIGTRAP)
#		endif
#	else
#		include <csignal>
#		define BREAKPOINT() raise(SIGTRAP)
#	endif
#endif // NDEBUG