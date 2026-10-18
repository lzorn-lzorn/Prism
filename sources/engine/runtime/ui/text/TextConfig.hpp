#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ui
{
enum class TextEncoding
{
    UTF8,
    UTF32,
    UTF16,
    Custom
};

enum class TextByteOrder { Native, LittleEndian, BigEndian };

// @note 只借用原始字节, 不推断编码；源数据必须在调用期间有效. 
template<class Character, class Traits, class Allocator>
std::string_view getTextBytes(const std::basic_string<Character, Traits, Allocator>& Text)
{
    return {reinterpret_cast<const char*>(Text.data()), Text.size() * sizeof(Character)};
}

struct TextConfig
{
    TextEncoding Encoding{TextEncoding::UTF8};
    std::function<std::string(std::string_view)> CustomDecoder{};
    TextByteOrder ByteOrder{TextByteOrder::Native};
    // @note 按顺序尝试缺字簇的回退字体, 不改变输入字节编码. 
    std::vector<std::string> FallbackFontPaths;
};

/**
 * @brief 通过 Unicode 工具转换为严格有效的 UTF-8, 保留内嵌 NUL 和 Unicode 标量的顺序. 
 * 不进行 Unicode 组合规范化, 也不替换格式错误的输入.
 * 输入为字节流；UTF16/UTF32 按配置字节序每两/四字节读取, 不自动剥离 BOM.
 * 无效编码、截断单元、非法 Unicode 或缺少解码器时抛出 invalid_argument.
 * 自定义解码器的异常直接传播, 其输出同样会被校验.
 * @param Text 待解码的文本内容.
 * @param Config 文本编码和自定义解码器配置.
 * @return 严格有效的 UTF-8 文本.
 */
std::string decodeText(std::string_view Text, const TextConfig& Config);
/**
 * @brief 通过 Unicode 工具将严格有效的 UTF-8 编码为配置指定的 UTF8/UTF16/UTF32 原始字节. 
 * 保留 NUL, 遵循字节序, 不添加 BOM；Custom 没有编码器, 调用时抛出 invalid_argument. 
 * @param UTF8 源 UTF-8 文本. 
 * @param Config 目标编码与字节序. 
 * @return 拥有存储的目标编码字节. 
 */
std::string encodeText(std::string_view UTF8, const TextConfig& Config);
template<class Character, class Traits, class Allocator>
std::string decodeText(const std::basic_string<Character, Traits, Allocator>& Text, const TextConfig& Config)
{
    return decodeText(getTextBytes(Text), Config);
}
}