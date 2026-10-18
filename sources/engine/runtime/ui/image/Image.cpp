#include "Image.hpp"
#include <algorithm>
#include <utility>

namespace ui
{
namespace
{
std::byte toByte(float Value) noexcept
{
    return static_cast<std::byte>(static_cast<unsigned char>(std::clamp(Value, 0.0f, 1.0f) * 255.0f));
}
}

Image::Image(std::uint32_t InWidth, std::uint32_t InHeight)
    : Width(InWidth), Height(InHeight)
{
    if (Width > 0 && Height > 0)
    {
        Pixels.resize(static_cast<std::size_t>(Width) * Height * 4);
    }
}

std::span<std::byte> Image::getPixels() noexcept
{
    return Pixels;
}

std::span<const std::byte> Image::getPixels() const noexcept
{
    return Pixels;
}

void Image::clear(Color Value)
{
    const std::byte R = toByte(Value.R);
    const std::byte G = toByte(Value.G);
    const std::byte B = toByte(Value.B);
    const std::byte A = toByte(Value.A);
    for (std::size_t Index = 0; Index < Pixels.size(); Index += 4)
    {
        Pixels[Index] = R;
        Pixels[Index + 1] = G;
        Pixels[Index + 2] = B;
        Pixels[Index + 3] = A;
    }
}

void Image::setMaterial(std::shared_ptr<IMaterial> Value) noexcept
{
    Material = std::move(Value);
}

std::shared_ptr<IMaterial> Image::getMaterial() const noexcept
{
    return Material;
}

UITextureData Image::toTextureData() const noexcept
{
    return UITextureData{Width, Height, Pixels};
}

}
