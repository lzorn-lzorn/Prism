#pragma once

#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>

// 错误码(按类别 + 消息), 全局空间.
enum class EErrorCode
{
    InvalidArgument,
    OutOfRange,
    Logic,
    Overflow,
    NotInitialized,
    Runtime,
    Io,
    Unsupported
};

// 统一错误类型, 全局空间; 继承 std::exception 保持现有 catch 兼容.
class Error : public std::exception
{
public:
    Error(EErrorCode Code, std::string Message,
          std::source_location Location = std::source_location::current());

    const char* what() const noexcept override;
    EErrorCode code() const noexcept;
    const std::source_location& location() const noexcept;

    // 统一抛出入口: 走 errorPolicy().OnError, 默认抛出本错误.
    [[noreturn]] static void raise(EErrorCode Code, std::string_view Message,
                                   std::source_location Location = std::source_location::current());
    // 运行时校验: Condition 为 false 时 raise.
    static void require(bool Condition, EErrorCode Code, std::string_view Message,
                        std::source_location Location = std::source_location::current());

private:
    EErrorCode Code;
    std::string Message;
    std::source_location Location;
};
