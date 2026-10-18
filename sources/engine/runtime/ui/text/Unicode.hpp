#pragma once

#include "TextConfig.hpp"

namespace ui
{
/**
 * @brief 严格转换 Unicode 编码, 保留内嵌 NUL、BOM 和标量顺序, 不进行规范化. 
 * 无效序列、截断单元、代理标量或超出 Unicode 范围时抛出 std::invalid_argument. 
 * UTF-16 和 UTF-32 参数为码元视图, 不涉及字节序；返回值拥有存储. 
 * @param Text 源编码文本, 存储必须在调用期间有效. 
 * @return 目标编码文本. 
 */
std::u32string utf8ToUTF32(std::string_view Text);
/** @brief 严格转换 UTF-16 为 UTF-32, 遵循上述转换契约.  */
std::u32string utf16ToUTF32(std::u16string_view Text);
/** @brief 严格转换 UTF-32 为 UTF-8, 遵循上述转换契约.  */
std::string utf32ToUTF8(std::u32string_view Text);
/** @brief 严格转换 UTF-32 为 UTF-16, 遵循上述转换契约.  */
std::u16string utf32ToUTF16(std::u32string_view Text);
/** @brief 严格转换 UTF-8 为 UTF-16, 遵循上述转换契约.  */
std::u16string utf8ToUTF16(std::string_view Text);
/** @brief 严格转换 UTF-16 为 UTF-8, 遵循上述转换契约.  */
std::string utf16ToUTF8(std::u16string_view Text);

/**
 * @brief 校验所有 UTF-32 码元均为 Unicode 标量, 允许 NUL 和非字符. 
 * @param Text 调用期间有效的 UTF-32 视图. 
 * @throws std::invalid_argument 存在代理标量或超出 Unicode 范围的码元. 
 */
void validateUTF32(std::u32string_view Text);

/**
 * @brief 按配置将原始字节严格解码为 UTF-32, 保留 NUL 和 BOM, 不进行规范化. 
 * UTF16/UTF32 按配置字节序读取；CustomDecoder 的 UTF-8 输出立即转换为 UTF-32. 
 * @param Text 调用期间有效的原始字节视图. 
 * @param Config 源编码、字节序和自定义解码器配置. 
 * @return 拥有存储的 UTF-32 文本. 
 * @throws std::invalid_argument 编码、字节序、序列无效或缺少自定义解码器. 
 * 自定义解码器的异常直接传播. 
 */
std::u32string decodeTextToUTF32(std::string_view Text, const TextConfig& Config);

/**
 * @brief 将 UTF-32 严格编码为配置指定的原始字节, 保留 NUL, 不添加 BOM 或进行规范化. 
 * @param Text 调用期间有效的 UTF-32 视图. 
 * @param Config 目标编码与字节序, UTF8 不使用字节序. 
 * @return 拥有存储的 UTF8/UTF16/UTF32 字节. 
 * @throws std::invalid_argument 标量、编码或字节序无效, 或目标编码为 Custom. 
 */
std::string encodeText(std::u32string_view Text, const TextConfig& Config);
}