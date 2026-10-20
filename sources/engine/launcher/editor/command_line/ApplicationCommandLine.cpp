#include "ApplicationCommandLine.hpp"
#include "CommandLineParser.hpp"
#include "GenericApplication.hpp"
#include <map>

namespace ui
{
void registerApplicationOptions(CommandLineParser& Parser, ApplicationOptions& Options)
{
    Parser.registerOption<int>("--frames", "运行帧数", [&Options](int Value)
        { Options.FrameLimit = Value; }, CLI::PositiveNumber);
    Parser.registerOption<std::string>("--font", "字体文件路径", [&Options](const std::string& Value)
        { Options.FontPath = Value; });

    const std::map<std::string, EGpuBackend> Backends{{"auto", EGpuBackend::Auto},
        {"vulkan", EGpuBackend::Vulkan}, {"metal", EGpuBackend::Metal},
        {"direct3d12", EGpuBackend::Direct3D12}};
    Parser.registerOption<EGpuBackend>("--backend", "GPU 后端", [&Options](EGpuBackend Value)
        { Options.Backend = Value; }, CLI::CheckedTransformer(Backends), CLI::IsMember(Backends));

    const std::map<std::string, TextEncoding> Encodings{{"utf8", TextEncoding::UTF8},
        {"utf16", TextEncoding::UTF16},
        {"utf32", TextEncoding::UTF32}};
    Parser.registerOption<TextEncoding>("--text-encoding", "文本编码", [&Options](TextEncoding Value)
        { Options.Text.Encoding = Value; }, CLI::CheckedTransformer(Encodings), CLI::IsMember(Encodings));

    const std::map<std::string, EUIMode> UIModes{{"retained", EUIMode::Retained},
        {"immediate", EUIMode::Immediate}};
    Parser.registerOption<EUIMode>("--ui-mode", "UI 渲染模式", [&Options](EUIMode Value)
        { Options.UIMode = Value; }, CLI::CheckedTransformer(UIModes), CLI::IsMember(UIModes));
}
}