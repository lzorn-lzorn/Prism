#pragma once

#include <infra/common/Error.hpp>
#include <functional>
#include <source_location>
#include <string_view>

// 中央错误策略: 决定"如何响应", 与逻辑解耦, 可全局覆盖.
struct ErrorPolicy
{
    std::function<void(const Error&, std::source_location)> OnError;     // 默认 throw
    std::function<void(std::string_view, std::source_location)> OnAssert; // 默认 log + abort
    std::function<void(const Error&, std::source_location)> OnFatal;     // 默认 log + abort
};

/**
 * @brief 返回进程内共享的错误策略；修改回调时调用者必须同步并发访问。
 * @return 策略引用。OnAssert 可返回；OnError 返回后仍抛出 Error，OnFatal 返回后仍终止。
 */
ErrorPolicy& errorPolicy();

namespace diag
{
void assertFailed(std::string_view Message, std::source_location Location);
[[noreturn]] void fatal(EErrorCode Code, std::string_view Message, std::source_location Location);
}

// 运行时校验: 始终求值, 失败抛 Error.
#define UI_CHECK(Condition, Code, Message) \
    ::Error::require((Condition), (Code), (Message))

// 致命错误: 不可恢复, 走 OnFatal.
#define UI_FATAL(Code, Message) \
    ::diag::fatal((Code), (Message), std::source_location::current())

// 断言: Debug 下失败走 OnAssert; Release 编译掉.
// 校验: 始终求值表达式; Debug 下同断言.
#ifndef NDEBUG
    #define UI_ASSERT(Condition, Message) \
        do { if (!(Condition)) { ::diag::assertFailed((Message), std::source_location::current()); } } while (0)
    #define UI_VERIFY(Condition, Message) UI_ASSERT((Condition), (Message))
#else
    #define UI_ASSERT(Condition, Message) ((void)0)
    #define UI_VERIFY(Condition, Message) ((void)(Condition))
#endif
