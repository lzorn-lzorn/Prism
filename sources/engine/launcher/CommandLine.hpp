#pragma once

class CommandLine
{
public:
    CommandLine(int argc, char** argv) {};
    ~CommandLine() = default;
    CommandLine(const CommandLine&) = delete;
    CommandLine& operator=(const CommandLine&) = delete;
    CommandLine(CommandLine&&) = delete;
    CommandLine& operator=(CommandLine&&) = delete;

};