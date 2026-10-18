#include <infra/common/Assert.hpp>
#include <cstdio>
#include <cstdlib>

namespace
{
void logFailure(std::string_view Kind, std::string_view Message, std::source_location Location)
{
    std::fwrite(Kind.data(), 1, Kind.size(), stderr);
    std::fputs(": ", stderr);
    std::fwrite(Message.data(), 1, Message.size(), stderr);
    std::fprintf(stderr, " [%s:%u, %s]\n", Location.file_name(), Location.line(), Location.function_name());
    std::fflush(stderr);
}
}

ErrorPolicy &errorPolicy()
{
    static ErrorPolicy Policy{
        [](const Error &Failure, std::source_location) { throw Failure; },
        [](std::string_view Message, std::source_location Location) {
            logFailure("Assertion failed", Message, Location);
            std::abort();
        },
        [](const Error &Failure, std::source_location Location) {
            logFailure("Fatal error", Failure.what(), Location);
            std::abort();
        }
    };
    return Policy;
}

void diag::assertFailed(std::string_view Message, std::source_location Location)
{
    if (errorPolicy().OnAssert)
        errorPolicy().OnAssert(Message, Location);
    else
    {
        logFailure("Assertion failed", Message, Location);
        std::abort();
    }
}

void diag::fatal(EErrorCode Code, std::string_view Message, std::source_location Location)
{
    Error Failure(Code, std::string(Message), Location);
    if (errorPolicy().OnFatal)
        errorPolicy().OnFatal(Failure, Location);
    logFailure("Fatal error", Message, Location);
    std::abort();
}