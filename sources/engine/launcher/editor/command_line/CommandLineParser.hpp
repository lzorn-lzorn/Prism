#pragma once

#include <CLI/CLI.hpp>
#include <functional>
#include <ostream>
#include <string>
#include <utility>

namespace ui
{
struct CommandLineResult
{
    bool ShouldExit{};
    int ExitCode{};
};

class CommandLineParser
{
public:
    explicit CommandLineParser(std::string Name, std::string Description = {})
        : App(std::move(Description), std::move(Name))
    {
        App.set_help_flag("-h,--help", "显示帮助信息");
    }

    /**
     * @brief 注册类型化选项；解析和校验通过后执行回调，重复传值报错。
     * @param Names 选项名称与别名，使用逗号分隔。
     * @param Description 帮助说明。
     * @param Callback 接收已转换值的回调。
     * @param Validator 输入校验规则。
     * @param InputValidator 类型转换前执行的原始输入校验规则。
     * @return 无返回值。
     */
    template<class Value>
    void registerOption(const std::string& Names, const std::string& Description,
        std::function<void(const Value&)> Callback, CLI::Validator Validator = {},
        CLI::Validator InputValidator = {})
    {
        App.add_option_function<Value>(Names, std::move(Callback), Description)
            ->transform(std::move(Validator))->transform(std::move(InputValidator))
            ->multi_option_policy(CLI::MultiOptionPolicy::Throw);
    }

    // @note 注册无值标志，回调仅在显式提供标志时执行；重复或覆盖标志报错。
    void registerFlag(const std::string& Names, const std::string& Description,
        std::function<void()> Callback);

    /**
     * @brief 解析启动参数；帮助和错误均返回退出结果，不终止进程。
     * @param Argc 包含程序名称的参数数量。
     * @param Argv 操作系统提供的参数数组。
     * @param Output 帮助输出流。
     * @param Errors 错误输出流。
     * @return 是否应退出以及退出码；成功解析时 ShouldExit 为 false。
     */
    CommandLineResult parse(int Argc, const char* const* Argv,
        std::ostream& Output, std::ostream& Errors);

private:
    CLI::App App;
};
}