#pragma once

#include "Image.hpp"
#include <cstddef>
#include <memory>
#include <span>
#include <string>

namespace ui
{
/**
 * @brief 通用图片导入模块: 将外部图片(文件/内存)解码为框架内统一的 Image 对象.
 * @note 本模块完全封装后端解码库(后端目前为 stb_image), 对外只暴露 Image;
 *       调用方既不感知、也不依赖具体解码库, 更换后端仅需修改本模块的 .cpp 实现, 头文件与调用方均无感.
 */
class ImageImporter
{
public:
    ImageImporter() = delete;

    /**
     * @brief 从磁盘文件解码一张图片, 输出为 CPU 侧 RGBA8 的 Image 对象.
     * @param Path 图片文件路径(UTF-8 编码), 须指向一个存在且格式受支持的文件
     *             (PNG / JPG / BMP / TGA / GIF / HDR / PNM 等); 空字符串或无法访问的文件将解码失败.
     * @return 成功: 返回非空 std::shared_ptr<Image>, 像素格式 RGBA8, 宽高等于图片像素尺寸;
     *         失败(文件不存在 / 不可读 / 格式不支持 / 数据损坏): 返回 nullptr.
     * @note 返回的 Image 拥有独立的像素缓冲, 与后端内部缓冲无共享, 可安全长期持有;
     *       解码过程不做任何缩放或格式转换(仅统一为 RGBA8 并转为 8 位).
     */
    static std::shared_ptr<Image> loadFromFile(const std::string& Path);

    /**
     * @brief 从内存字节流解码一张图片, 输出为 CPU 侧 RGBA8 的 Image 对象.
     * @param Data 指向图片完整文件字节的只读区间(任意受支持格式); 空区间必然失败,
     *             区间长度需不超过 int 上限(通常图片远小于此).
     * @return 成功: 返回非空 std::shared_ptr<Image>, 像素格式 RGBA8, 宽高等于图片像素尺寸;
     *         失败(空数据 / 格式不支持 / 数据损坏): 返回 nullptr.
     * @note 语义与 loadFromFile 一致, 仅数据来源不同; Data 只需在调用期间有效, 解码后即被复制.
     */
    static std::shared_ptr<Image> loadFromMemory(std::span<const std::byte> Data);
};

}
