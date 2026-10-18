#pragma once

namespace ui
{
class CommandLineParser;
struct ApplicationOptions;

// @note 将应用选项和配置写入回调注册到解析器，配置须存活到解析结束。
void registerApplicationOptions(CommandLineParser& Parser, ApplicationOptions& Options);
}