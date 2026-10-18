#include "ImageImporter.hpp"

// 后端: stb_image(单头文件库), 仅在本翻译单元内展开实现, 不泄漏到对外头文件.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstring>

namespace ui
{
std::shared_ptr<Image> ImageImporter::loadFromFile(const std::string& Path)
{
    if (Path.empty())
    {
        return nullptr;
    }

    int Width = 0;
    int Height = 0;
    int Channels = 0;
    // 强制解码为 4 通道(RGBA), 与 Image 的 RGBA8 表示一致; 无 alpha 的来源会补为不透明.
    stbi_uc* Data = stbi_load(Path.c_str(), &Width, &Height, &Channels, 4);
    if (!Data)
    {
        return nullptr;
    }

    auto Result = std::make_shared<Image>(static_cast<std::uint32_t>(Width), static_cast<std::uint32_t>(Height));
    std::memcpy(Result->getPixels().data(), Data, static_cast<std::size_t>(Width) * Height * 4);
    stbi_image_free(Data);
    return Result;
}

std::shared_ptr<Image> ImageImporter::loadFromMemory(std::span<const std::byte> Data)
{
    if (Data.empty())
    {
        return nullptr;
    }

    int Width = 0;
    int Height = 0;
    int Channels = 0;
    const auto* Bytes = reinterpret_cast<const stbi_uc*>(Data.data());
    stbi_uc* Pixels = stbi_load_from_memory(Bytes, static_cast<int>(Data.size()), &Width, &Height, &Channels, 4);
    if (!Pixels)
    {
        return nullptr;
    }

    auto Result = std::make_shared<Image>(static_cast<std::uint32_t>(Width), static_cast<std::uint32_t>(Height));
    std::memcpy(Result->getPixels().data(), Pixels, static_cast<std::size_t>(Width) * Height * 4);
    stbi_image_free(Pixels);
    return Result;
}

}
