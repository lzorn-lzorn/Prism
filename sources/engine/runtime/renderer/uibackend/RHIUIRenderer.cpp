#include "RHIUIRenderer.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ui
{
namespace
{
std::uint64_t AllocateTextureHandle()
{
    static std::atomic<std::uint64_t> Next { 1 };
    std::uint64_t Value = Next.load(std::memory_order_relaxed);
    for (;;)
    {
        if (Value == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("UI texture handle space exhausted");
        if (Next.compare_exchange_weak(Value, Value + 1, std::memory_order_relaxed))
            return Value;
    }
}
} // namespace

struct RHIUIRenderer::Impl
{
    struct TextureRecord
    {
        std::uint32_t Width {};
        std::uint32_t Height {};
        std::vector<std::byte> Pixels;

        /** @brief 是否存在尚未被渲染侧取走的像素修改. */
        bool Dirty { false };
        /** @brief 自上次取走后修改像素的并集矩形(紧密排列的行). */
        std::uint32_t DirtyX {};
        std::uint32_t DirtyY {};
        std::uint32_t DirtyWidth {};
        std::uint32_t DirtyHeight {};
    };

    EUIMode Mode { EUIMode::Retained };
    UIFrameInfo Frame {};
    bool Active { false };
    std::vector<UIDrawList> DrawLists;
    std::unordered_map<std::uint64_t, TextureRecord> Textures;
};

RHIUIRenderer::RHIUIRenderer()
    : ImplPtr(std::make_unique<Impl>())
{
}

RHIUIRenderer::~RHIUIRenderer() = default;

bool RHIUIRenderer::canBeginFrame(const UIFrameInfo& InFrame)
{
    ImplPtr->Frame = InFrame;
    ImplPtr->DrawLists.clear();
    ImplPtr->Active = true;
    return true;
}

void RHIUIRenderer::submitDrawList(const UIDrawList& DrawList, bool /*ReuseBuffers*/)
{
    if (!ImplPtr->Active)
        return;
    // 复制提交: 调用方随后可复用/清空; 渲染线程消费副本.
    ImplPtr->DrawLists.push_back(DrawList);
}

void RHIUIRenderer::endFrame()
{
    ImplPtr->Active = false;
}

EUIMode RHIUIRenderer::getUIMode() const noexcept
{
    return ImplPtr->Mode;
}

void RHIUIRenderer::setUIMode(EUIMode Mode) noexcept
{
    ImplPtr->Mode = Mode;
}

TextureHandle RHIUIRenderer::createTexture(const UITextureData& Data)
{
    const std::uint64_t Handle = AllocateTextureHandle();
    Impl::TextureRecord Record;
    Record.Width = Data.Width;
    Record.Height = Data.Height;
    Record.Pixels.assign(Data.Pixels.begin(), Data.Pixels.end());
    // 新纹理: 整张都要上传(渲染侧首次见到该句柄).
    Record.Dirty = Data.Width != 0 && Data.Height != 0;
    Record.DirtyX = 0;
    Record.DirtyY = 0;
    Record.DirtyWidth = Data.Width;
    Record.DirtyHeight = Data.Height;
    ImplPtr->Textures.emplace(Handle, std::move(Record));
    return TextureHandle { Handle };
}

void RHIUIRenderer::updateTexture(TextureHandle Texture, const UITextureUpdate& Update)
{
    const auto It = ImplPtr->Textures.find(Texture.Value);
    if (It == ImplPtr->Textures.end())
        return;
    Impl::TextureRecord& Record = It->second;
    const std::size_t Stride = 4;
    for (std::uint32_t Row = 0; Row < Update.Height; ++Row)
    {
        const std::size_t Dst = (static_cast<std::size_t>(Update.Y) + Row) * Record.Width * Stride +
            static_cast<std::size_t>(Update.X) * Stride;
        const std::size_t Src = static_cast<std::size_t>(Row) * Update.Width * Stride;
        const std::size_t Bytes = static_cast<std::size_t>(Update.Width) * Stride;
        if (Dst + Bytes > Record.Pixels.size())
            return;
        std::copy_n(Update.Pixels.begin() + static_cast<std::ptrdiff_t>(Src),
            static_cast<std::ptrdiff_t>(Bytes), Record.Pixels.begin() + static_cast<std::ptrdiff_t>(Dst));
    }
    if (Update.Width == 0 || Update.Height == 0)
        return;
    // 与已有脏区求并集(字形逐个入库, 单帧内可能命中多个小块).
    if (!Record.Dirty)
    {
        Record.Dirty = true;
        Record.DirtyX = Update.X;
        Record.DirtyY = Update.Y;
        Record.DirtyWidth = Update.Width;
        Record.DirtyHeight = Update.Height;
        return;
    }
    const std::uint32_t Left = std::min(Record.DirtyX, Update.X);
    const std::uint32_t Top = std::min(Record.DirtyY, Update.Y);
    const std::uint32_t Right = std::max(Record.DirtyX + Record.DirtyWidth, Update.X + Update.Width);
    const std::uint32_t Bottom = std::max(Record.DirtyY + Record.DirtyHeight, Update.Y + Update.Height);
    Record.DirtyX = Left;
    Record.DirtyY = Top;
    Record.DirtyWidth = Right - Left;
    Record.DirtyHeight = Bottom - Top;
}

void RHIUIRenderer::destroyTexture(TextureHandle Texture)
{
    ImplPtr->Textures.erase(Texture.Value);
}

const std::vector<UIDrawList>& RHIUIRenderer::getDrawLists() const noexcept
{
    return ImplPtr->DrawLists;
}

const UITextureData* RHIUIRenderer::getTextureData(TextureHandle Texture) const noexcept
{
    const auto It = ImplPtr->Textures.find(Texture.Value);
    if (It == ImplPtr->Textures.end())
        return nullptr;
    const Impl::TextureRecord& Record = It->second;
    // 每次查询返回临时的 span 视图; 调用方必须立即消费, 不得跨帧持有.
    thread_local UITextureData Data;
    Data.Width = Record.Width;
    Data.Height = Record.Height;
    Data.Pixels = Record.Pixels;
    return &Data;
}

std::vector<UISampledTextureUpload> RHIUIRenderer::collectTextureUploads()
{
    std::vector<UISampledTextureUpload> Uploads;
    Uploads.reserve(ImplPtr->Textures.size());
    for (auto& [Handle, Record] : ImplPtr->Textures)
    {
        if (!Record.Dirty || Record.DirtyWidth == 0 || Record.DirtyHeight == 0 ||
            Record.Width == 0 || Record.Height == 0)
            continue;
        // 脏区可能被上游裁剪到图内, 这里再兜一次边界.
        const std::uint32_t Left = std::min(Record.DirtyX, Record.Width);
        const std::uint32_t Top = std::min(Record.DirtyY, Record.Height);
        const std::uint32_t Width = std::min(Record.DirtyWidth, Record.Width - Left);
        const std::uint32_t Height = std::min(Record.DirtyHeight, Record.Height - Top);
        Record.Dirty = false;
        if (Width == 0 || Height == 0)
            continue;

        UISampledTextureUpload Upload;
        Upload.Handle = Handle;
        Upload.Width = Record.Width;
        Upload.Height = Record.Height;
        Upload.X = Left;
        Upload.Y = Top;
        Upload.UploadWidth = Width;
        Upload.UploadHeight = Height;
        Upload.Full = Left == 0 && Top == 0 && Width == Record.Width && Height == Record.Height;

        const std::size_t RowBytes = static_cast<std::size_t>(Width) * 4;
        Upload.Pixels.resize(RowBytes * Height);
        for (std::uint32_t Row = 0; Row < Height; ++Row)
        {
            const std::size_t Source =
                (static_cast<std::size_t>(Top + Row) * Record.Width + Left) * 4;
            if (Source + RowBytes > Record.Pixels.size())
                break;
            std::copy_n(Record.Pixels.begin() + static_cast<std::ptrdiff_t>(Source),
                static_cast<std::ptrdiff_t>(RowBytes),
                Upload.Pixels.begin() + static_cast<std::ptrdiff_t>(Row * RowBytes));
        }
        Uploads.push_back(std::move(Upload));
    }
    return Uploads;
}

} // namespace ui
