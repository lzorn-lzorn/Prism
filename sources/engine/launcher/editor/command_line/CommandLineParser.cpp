#include "CommandLineParser.hpp"

namespace ui
{
void CommandLineParser::registerFlag(const std::string& Names, const std::string& Description,
    std::function<void()> Callback)
{
    App.add_flag_function(Names, [Action = std::move(Callback)](std::int64_t) { Action(); }, Description)
        ->disable_flag_override()->multi_option_policy(CLI::MultiOptionPolicy::Throw);
}

CommandLineResult CommandLineParser::parse(int Argc, const char* const* Argv,
    std::ostream& Output, std::ostream& Errors)
{
    try
    {
        App.parse(Argc, Argv);
        return {};
    }
    catch (const CLI::ParseError& Error)
    {
        return {true, App.exit(Error, Output, Errors)};
    }
}
}