#include <infra/common/Assert.hpp>
#include <utility>

Error::Error(EErrorCode InCode, std::string InMessage, std::source_location InLocation)
    : Code(InCode), Message(std::move(InMessage)), Location(InLocation)
{
}

const char *Error::what() const noexcept
{
    return Message.c_str();
}

EErrorCode Error::code() const noexcept
{
    return Code;
}

const std::source_location &Error::location() const noexcept
{
    return Location;
}

void Error::raise(EErrorCode Code, std::string_view Message, std::source_location Location)
{
    Error Failure(Code, std::string(Message), Location);
    if (errorPolicy().OnError)
        errorPolicy().OnError(Failure, Location);
    throw Failure;
}

void Error::require(bool Condition, EErrorCode Code, std::string_view Message,
                    std::source_location Location)
{
    if (!Condition)
        raise(Code, Message, Location);
}