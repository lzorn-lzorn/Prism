#include <infra/common/Error.hpp>
#include "FontService.hpp"
#include "TextLayoutEngine.hpp"
#include "Unicode.hpp"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace ui
{
namespace
{
constexpr std::uint32_t AtlasSize = 1024;
std::atomic<std::uint64_t> NextFont{1};

std::uint64_t allocateFontHandle()
{
    auto Value = NextFont.load(std::memory_order_relaxed);
    for (;;)
    {
        if (Value == std::numeric_limits<std::uint64_t>::max())
        {
            Error::raise(EErrorCode::Overflow, "Font handle space exhausted");
        }
        if (NextFont.compare_exchange_weak(Value, Value + 1, std::memory_order_relaxed))
        {
            return Value;
        }
    }
}

bool isWrapSpace(std::uint32_t Value)
{
    return Value == ' ' || Value == '\t' || Value == 0x1680
        || (Value >= 0x2000 && Value <= 0x2006)
        || (Value >= 0x2008 && Value <= 0x200a)
        || Value == 0x205f || Value == 0x3000;
}

struct ShapedGlyph
{
    hb_glyph_info_t Info;
    hb_glyph_position_t Position;
    std::uint64_t Font{};
};

struct ShapedRun
{
    std::vector<ShapedGlyph> Glyphs;
    hb_segment_properties_t Properties{};
    float Advance{};
};

ShapedRun shape(hb_font_t* Font, std::u32string_view Text, std::uint32_t Begin,
                std::uint32_t End, const hb_segment_properties_t* Properties = nullptr)
{
    std::unique_ptr<hb_buffer_t, decltype(&hb_buffer_destroy)> Buffer(hb_buffer_create(), hb_buffer_destroy);
    hb_buffer_set_cluster_level(Buffer.get(), HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES);
    hb_buffer_set_content_type(Buffer.get(), HB_BUFFER_CONTENT_TYPE_UNICODE);
    for (auto Index = Begin; Index < End; ++Index)
    {
        hb_buffer_add(Buffer.get(), static_cast<hb_codepoint_t>(Text[Index]), Index - Begin);
    }
    if (Properties)
    {
        hb_buffer_set_segment_properties(Buffer.get(), Properties);
    }
    else
    {
        hb_buffer_guess_segment_properties(Buffer.get());
    }
    hb_shape(Font, Buffer.get(), nullptr, 0);
    if (!hb_buffer_allocation_successful(Buffer.get()))
    {
        Error::raise(EErrorCode::Runtime, "HarfBuzz buffer allocation failed");
    }
    ShapedRun Result;
    hb_buffer_get_segment_properties(Buffer.get(), &Result.Properties);
    unsigned int Count = 0;
    const auto* Infos = hb_buffer_get_glyph_infos(Buffer.get(), &Count);
    const auto* Positions = hb_buffer_get_glyph_positions(Buffer.get(), nullptr);
    Result.Glyphs.reserve(Count);
    for (unsigned int Index = 0; Index < Count; ++Index)
    {
        auto Info = Infos[Index];
        Info.cluster += Begin;
        Result.Glyphs.push_back({Info, Positions[Index]});
        Result.Advance += static_cast<float>(Positions[Index].x_advance) / 64.0f;
    }
    return Result;
}
}

Vector2 TextLayout::measure(Vector2 AvailableSize) const
{
    validateLayoutSize(AvailableSize);
    const Vector2 Desired{Metrics.Width, Metrics.Height};
    validateLayoutSize(Desired);
    return Desired;
}

void TextLayout::arrange(UIRect Bounds)
{
    validateLayoutBounds(Bounds);
    Allocation = Bounds;
}

UIRect TextLayout::getBounds() const noexcept
{
    return Allocation;
}

const TextMetrics& TextLayout::getMetrics() const noexcept
{
    return Metrics;
}

std::uint32_t TextLayout::hitTest(UIPoint Point) const noexcept
{
    if (Carets.empty() || !std::isfinite(Point.X) || !std::isfinite(Point.Y))
    {
        return 0;
    }
    const TextCaret* Best = &Carets.front();
    float BestVertical = std::numeric_limits<float>::infinity();
    float BestHorizontal = std::numeric_limits<float>::infinity();
    for (const auto& Caret : Carets)
    {
        const auto Center = Caret.Bounds.Y + Caret.Bounds.Height * 0.5f;
        const auto Vertical = std::abs(Point.Y - Center);
        const auto Horizontal = std::abs(Point.X - Caret.Bounds.X);
        if (Vertical < BestVertical || (Vertical == BestVertical && Horizontal < BestHorizontal))
        {
            Best = &Caret;
            BestVertical = Vertical;
            BestHorizontal = Horizontal;
        }
    }
    return Best->Offset;
}

UIRect TextLayout::getCaretRect(std::uint32_t Offset) const noexcept
{
    const TextCaret* Best = nullptr;
    for (const auto& Caret : Carets)
    {
        if (Caret.Offset <= Offset && (!Best || Caret.Offset >= Best->Offset))
        {
            Best = &Caret;
        }
    }
    return Best ? Best->Bounds : UIRect{};
}

struct FontService::Impl
{
    struct FontRecord
    {
        FT_Face Face{};
        hb_font_t* Font{};
        std::vector<FontHandle> Fallbacks;
        float BitmapScale{1};

        void setSize(std::uint32_t Size)
        {
            BitmapScale = 1;
            if (FT_IS_SCALABLE(Face))
            {
                if (FT_Set_Char_Size(Face, 0, Size, 72, 72))
                    Error::raise(EErrorCode::Runtime, "Unable to set font size");
            }
            else
            {
                int Best = 0;
                for (int Index = 1; Index < Face->num_fixed_sizes; ++Index)
                {
                    if (std::abs(static_cast<double>(Face->available_sizes[Index].y_ppem) - Size) <
                        std::abs(static_cast<double>(Face->available_sizes[Best].y_ppem) - Size)) Best = Index;
                }
                if (FT_Select_Size(Face, Best) || Face->size->metrics.y_ppem == 0)
                    Error::raise(EErrorCode::Runtime, "Unable to select bitmap font size");
                BitmapScale = static_cast<float>(Size) / (64 * Face->size->metrics.y_ppem);
            }
            hb_ft_font_changed(Font);
        }

        ~FontRecord()
        {
            if (Font)
            {
                hb_font_destroy(Font);
            }
            if (Face)
            {
                FT_Done_Face(Face);
            }
        }
    };

    struct Page
    {
        TextureHandle Texture;
        std::vector<std::byte> Pixels;
        std::uint32_t NextX{1}, NextY{1}, ShelfHeight{};
    };

    struct CachedGlyph
    {
        TextureHandle Texture;
        UIRect UV;
        float Left{}, Top{}, Width{}, Height{};
        bool Colored{};
    };

    IUIRenderer& Renderer;
    FT_Library Library{};
    std::map<std::uint64_t, std::unique_ptr<FontRecord>> Fonts;
    std::map<std::string, FontHandle> FontPaths;
    std::vector<Page> Pages;
    std::map<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>, CachedGlyph> Cache;

    explicit Impl(IUIRenderer& RendererValue) : Renderer(RendererValue)
    {
        if (FT_Init_FreeType(&Library))
        {
            Error::raise(EErrorCode::Runtime, "FreeType initialization failed");
        }
    }

    ~Impl() noexcept
    {
        for (const auto& PageValue : Pages)
        {
            try
            {
                Renderer.destroyTexture(PageValue.Texture);
            }
            catch (...)
            {
            }
        }
        Fonts.clear();
        FT_Done_FreeType(Library);
    }

    CachedGlyph rasterize(std::uint64_t Handle, FontRecord& Font, std::uint32_t Glyph,
                          std::uint32_t PixelSize)
    {
        const auto Key = std::make_tuple(Handle, Glyph, PixelSize);
        if (const auto Found = Cache.find(Key); Found != Cache.end())
        {
            return Found->second;
        }
        if (FT_Load_Glyph(Font.Face, Glyph, FT_LOAD_NO_HINTING | FT_LOAD_COLOR)
            || (Font.Face->glyph->format != FT_GLYPH_FORMAT_BITMAP &&
                FT_Render_Glyph(Font.Face->glyph, FT_RENDER_MODE_NORMAL)))
        {
            Error::raise(EErrorCode::Runtime, "FreeType glyph rasterization failed");
        }
        const auto& Bitmap = Font.Face->glyph->bitmap;
        CachedGlyph Result;
        Result.Left = static_cast<float>(Font.Face->glyph->bitmap_left) * Font.BitmapScale;
        Result.Top = static_cast<float>(Font.Face->glyph->bitmap_top) * Font.BitmapScale;
        const auto Width = static_cast<std::uint32_t>(std::max(1.0f, std::round(Bitmap.width * Font.BitmapScale)));
        const auto Height = static_cast<std::uint32_t>(std::max(1.0f, std::round(Bitmap.rows * Font.BitmapScale)));
        Result.Width = static_cast<float>(Width);
        Result.Height = static_cast<float>(Height);
        if (!Bitmap.width || !Bitmap.rows)
        {
            Result.Width = Result.Height = 0;
            Cache.emplace(Key, Result);
            return Result;
        }
        Result.Colored = Bitmap.pixel_mode == FT_PIXEL_MODE_BGRA;
        if (Bitmap.pixel_mode != FT_PIXEL_MODE_GRAY && Bitmap.pixel_mode != FT_PIXEL_MODE_MONO && !Result.Colored)
        {
            Error::raise(EErrorCode::Runtime, "Unsupported FreeType bitmap format");
        }
        if (Width > AtlasSize - 2 || Height > AtlasSize - 2)
        {
            Error::raise(EErrorCode::Runtime, "Glyph exceeds atlas page size");
        }
        const auto PatchWidth = Width + 2;
        const auto PatchHeight = Height + 2;
        std::vector<std::byte> Patch(static_cast<size_t>(PatchWidth) * PatchHeight * 4);
        for (std::uint32_t Row = 0; Row < Height; ++Row)
        {
            const auto Pitch = static_cast<std::int64_t>(Bitmap.pitch);
            const auto SampleRow = static_cast<std::uint32_t>(static_cast<std::uint64_t>(Row) * Bitmap.rows / Height);
            const auto SourceRow = Pitch < 0 ? Bitmap.rows - SampleRow - 1 : SampleRow;
            const auto* Source = Bitmap.buffer + SourceRow * std::abs(Pitch);
            for (std::uint32_t Column = 0; Column < Width; ++Column)
            {
                const auto SampleColumn = static_cast<std::uint32_t>(static_cast<std::uint64_t>(Column) * Bitmap.width / Width);
                const auto Pixel = (static_cast<size_t>(Row + 1) * PatchWidth + Column + 1) * 4;
                if (Result.Colored)
                {
                    const auto* ColorPixel = Source + SampleColumn * 4;
                    const unsigned int Alpha = ColorPixel[3];
                    for (size_t Channel = 0; Channel < 3; ++Channel)
                    {
                        Patch[Pixel + Channel] = static_cast<std::byte>(Alpha == 0 ? 0 :
                            std::min(255u, (ColorPixel[2 - Channel] * 255u + Alpha / 2) / Alpha));
                    }
                    Patch[Pixel + 3] = static_cast<std::byte>(Alpha);
                    continue;
                }
                unsigned int Coverage = 0;
                if (Bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
                {
                    Coverage = (Source[SampleColumn / 8] & (0x80 >> (SampleColumn % 8))) ? 255 : 0;
                }
                else
                {
                    if (Bitmap.num_grays < 2)
                    {
                        Error::raise(EErrorCode::Runtime, "Invalid grayscale bitmap");
                    }
                    Coverage = std::min(255u, static_cast<unsigned int>(Source[SampleColumn]) * 255u
                                              / (Bitmap.num_grays - 1u));
                }
                Patch[Pixel] = Patch[Pixel + 1] = Patch[Pixel + 2] = std::byte{255};
                Patch[Pixel + 3] = static_cast<std::byte>(Coverage);
            }
        }

        Page* Target = nullptr;
        std::uint32_t TargetX = 0, TargetY = 0;
        for (auto& PageValue : Pages)
        {
            auto NextX = PageValue.NextX;
            auto NextY = PageValue.NextY;
            if (NextX + PatchWidth > AtlasSize)
            {
                NextX = 1;
                NextY += PageValue.ShelfHeight;
            }
            if (NextY + PatchHeight <= AtlasSize)
            {
                Target = &PageValue;
                TargetX = NextX;
                TargetY = NextY;
                break;
            }
        }
        if (!Target)
        {
            Page NewPage;
            NewPage.Pixels.resize(static_cast<size_t>(AtlasSize) * AtlasSize * 4);
            Pages.reserve(Pages.size() + 1);
            NewPage.Texture = Renderer.createTexture({AtlasSize, AtlasSize, NewPage.Pixels});
            if (!NewPage.Texture.Value)
            {
                Error::raise(EErrorCode::Runtime, "Renderer returned invalid atlas handle");
            }
            Pages.push_back(std::move(NewPage));
            Target = &Pages.back();
            TargetX = TargetY = 1;
        }
        Renderer.updateTexture(Target->Texture, {TargetX, TargetY, PatchWidth, PatchHeight, Patch});
        for (std::uint32_t Row = 0; Row < PatchHeight; ++Row)
        {
            const auto Source = Patch.begin() + static_cast<std::ptrdiff_t>(Row * PatchWidth * 4);
            const auto Destination = Target->Pixels.begin()
                + static_cast<std::ptrdiff_t>((static_cast<size_t>(TargetY + Row) * AtlasSize + TargetX) * 4);
            std::copy_n(Source, PatchWidth * 4, Destination);
        }
        if (TargetY != Target->NextY)
        {
            Target->ShelfHeight = 0;
        }
        Target->NextX = TargetX + PatchWidth;
        Target->NextY = TargetY;
        Target->ShelfHeight = std::max(Target->ShelfHeight, PatchHeight);
        Result.Texture = Target->Texture;
        Result.UV = {static_cast<float>(TargetX + 1) / AtlasSize, static_cast<float>(TargetY + 1) / AtlasSize,
                     Result.Width / AtlasSize, Result.Height / AtlasSize};
        Cache.emplace(Key, Result);
        return Result;
    }

    void appendLine(TextLayout& IVisual, FontRecord& Font, std::uint64_t Handle,
                    std::uint32_t PixelSize, float Scale, const ShapedRun& Run,
                    std::uint32_t Begin, std::uint32_t End, float Top)
    {
        const bool RTL = HB_DIRECTION_IS_BACKWARD(Run.Properties.direction);
        const auto Width = std::abs(Run.Advance) / Scale;
        auto PenX = Run.Advance < 0 ? Width : 0.0f;
        float PenY = 0;
        std::map<std::uint32_t, std::pair<float, float>> Clusters;
        for (const auto& Glyph : Run.Glyphs)
        {
            const auto NextX = PenX + static_cast<float>(Glyph.Position.x_advance) / 64.0f / Scale;
            const auto Left = std::min(PenX, NextX);
            const auto Right = std::max(PenX, NextX);
            const auto [Found, Inserted] = Clusters.emplace(Glyph.Info.cluster, std::make_pair(Left, Right));
            if (!Inserted)
            {
                Found->second.first = std::min(Found->second.first, Left);
                Found->second.second = std::max(Found->second.second, Right);
            }
            const auto GlyphHandle = Glyph.Font == 0 ? Handle : Glyph.Font;
            auto& GlyphFont = Glyph.Font == 0 ? Font : *Fonts.at(Glyph.Font);
            const auto Bitmap = rasterize(GlyphHandle, GlyphFont, Glyph.Info.codepoint, PixelSize);
            if (Bitmap.Width && Bitmap.Height)
            {
                const UIRect Bounds{
                    PenX + (static_cast<float>(Glyph.Position.x_offset) / 64.0f + Bitmap.Left) / Scale,
                    Top + IVisual.Metrics.Ascent - PenY
                        - (static_cast<float>(Glyph.Position.y_offset) / 64.0f + Bitmap.Top) / Scale,
                    Bitmap.Width / Scale, Bitmap.Height / Scale};
                IVisual.Glyphs.push_back({Bitmap.Texture, Bounds, Bitmap.UV, Glyph.Info.cluster, Bitmap.Colored});
            }
            PenX = NextX;
            PenY += static_cast<float>(Glyph.Position.y_advance) / 64.0f / Scale;
        }
        const auto addCaret = [&](std::uint32_t Offset, float Position)
        {
            IVisual.Carets.push_back({Offset, {Position, Top, 1.0f / Scale, IVisual.Metrics.LineHeight}});
        };
        if (!Clusters.contains(Begin))
        {
            addCaret(Begin, RTL ? Width : 0);
        }
        for (const auto& [Cluster, Extent] : Clusters)
        {
            addCaret(Cluster, RTL ? Extent.second : Extent.first);
        }
        if (End != Begin || !Clusters.empty())
        {
            addCaret(End, RTL ? 0 : Width);
        }
        IVisual.Metrics.Width = std::max(IVisual.Metrics.Width, Width);
        IVisual.Metrics.Height = Top + IVisual.Metrics.LineHeight;
    }
};

FontService& FontService::self()
{
    static FontService Instance;
    return Instance;
}

FontService::FontService() = default;
FontService::~FontService() noexcept = default;

void FontService::setDefaultTextConfig(const TextConfig& Value)
{
    DefaultTextConfig = Value;
}

const TextConfig& FontService::getDefaultTextConfig() const noexcept
{
    return DefaultTextConfig;
}

void FontService::setDefaultFont(FontHandle Value) noexcept
{
    DefaultFont = Value;
}

FontHandle FontService::getDefaultFont() const noexcept
{
    return DefaultFont;
}

void FontService::setDefaultFallbackFonts(std::vector<FontHandle> Value)
{
    DefaultFallbacks = std::move(Value);
}

const std::vector<FontHandle>& FontService::getDefaultFallbackFonts() const noexcept
{
    return DefaultFallbacks;
}

std::unique_ptr<TextLayoutEngine> FontService::createTextLayoutEngine()
{
    return std::unique_ptr<TextLayoutEngine>(new TextLayoutEngine(DefaultTextConfig, DefaultFont, DefaultFallbacks));
}

void FontService::initialize(IUIRenderer& Renderer)
{
    if (State)
    {
        if (&State->Renderer != &Renderer)
        {
            Error::raise(EErrorCode::Logic, "Text service is bound to a different renderer");
        }
        return;
    }
    auto NewState = std::make_unique<Impl>(Renderer);
    State = std::move(NewState);
}

void FontService::initialize(std::shared_ptr<IUIRenderer> Renderer)
{
    if (!Renderer)
    {
        Error::raise(EErrorCode::InvalidArgument, "Text service renderer must not be null");
    }
    initialize(*Renderer);
    if (!RendererOwner)
    {
        RendererOwner = std::move(Renderer);
    }
}

bool FontService::isInitialized() const noexcept
{
    return State != nullptr;
}

void FontService::shutdown() noexcept
{
    State.reset();
    RendererOwner.reset();
}

void FontService::checkInitialized() const
{
    if (!isInitialized())
    {
        Error::raise(EErrorCode::Logic, "Text service is not initialized");
    }
}

FontHandle FontService::loadFont(const std::string& Path)
{
    checkInitialized();
    if (Path.empty() || Path.find('\0') != std::string::npos)
    {
        Error::raise(EErrorCode::InvalidArgument, "Invalid font path");
    }
    if (const auto Found = State->FontPaths.find(Path); Found != State->FontPaths.end())
    {
        return Found->second;
    }
    auto Record = std::make_unique<Impl::FontRecord>();
    if (FT_New_Face(State->Library, Path.c_str(), 0, &Record->Face))
    {
        Error::raise(EErrorCode::Runtime, "Unable to load font: " + Path);
    }
    if ((!FT_IS_SCALABLE(Record->Face) && !FT_HAS_FIXED_SIZES(Record->Face)) ||
        FT_Select_Charmap(Record->Face, FT_ENCODING_UNICODE))
    {
        Error::raise(EErrorCode::Runtime, "A scalable or fixed-size Unicode font is required");
    }
    Record->Font = hb_ft_font_create_referenced(Record->Face);
    if (!Record->Font || Record->Font == hb_font_get_empty())
    {
        Error::raise(EErrorCode::Runtime, "Unable to create HarfBuzz font");
    }
    hb_ft_font_set_load_flags(Record->Font, FT_LOAD_NO_HINTING | FT_LOAD_COLOR);
    const auto Handle = allocateFontHandle();
    State->Fonts.emplace(Handle, std::move(Record));
    try
    {
        State->FontPaths.emplace(Path, FontHandle{Handle});
    }
    catch (...)
    {
        State->Fonts.erase(Handle);
        throw;
    }
    return {Handle};
}

void FontService::setFallbackFonts(FontHandle Font, std::vector<FontHandle> Fallbacks)
{
    checkInitialized();
    const auto Primary = State->Fonts.find(Font.Value);
    if (Primary == State->Fonts.end()) Error::raise(EErrorCode::InvalidArgument, "Invalid primary font handle");
    for (const auto Fallback : Fallbacks)
    {
        if (Fallback == Font || !State->Fonts.contains(Fallback.Value))
            Error::raise(EErrorCode::InvalidArgument, "Invalid fallback font handle");
    }
    Primary->second->Fallbacks = std::move(Fallbacks);
}

TextLayout FontService::createTextLayout(FontHandle Font, std::u32string_view UTF32, float Size,
                                       float PixelScale, float MaxWidth)
{
    checkInitialized();
    const auto Found = State->Fonts.find(Font.Value);
    if (Found == State->Fonts.end())
    {
        Error::raise(EErrorCode::InvalidArgument, "Invalid or foreign font handle");
    }
    const auto Pixels = static_cast<double>(Size) * PixelScale;
    if (!std::isfinite(Size) || Size <= 0 || Size > 256 || !std::isfinite(PixelScale)
        || PixelScale <= 0 || !std::isfinite(Pixels) || Pixels > 512
        || !std::isfinite(MaxWidth) || MaxWidth < 0)
    {
        Error::raise(EErrorCode::InvalidArgument, "Invalid text size, scale or wrap width");
    }
    const auto PixelSize = static_cast<std::uint32_t>(std::max(1.0, std::round(Pixels * 64)));
    if (UTF32.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        Error::raise(EErrorCode::InvalidArgument, "Text exceeds HarfBuzz length limits");
    }
    validateUTF32(UTF32);
    auto& Record = *Found->second;
    Record.setSize(PixelSize);
    for (const auto Fallback : Record.Fallbacks)
    {
        auto& Face = *State->Fonts.at(Fallback.Value);
        Face.setSize(PixelSize);
    }
    TextLayout IVisual;
    IVisual.Text = std::u32string(UTF32);
    const auto Shape = [&](std::uint32_t Start, std::uint32_t End, const hb_segment_properties_t* Properties = nullptr)
    {
        auto Run = shape(Record.Font, IVisual.Text, Start, End, Properties);
        if (Record.BitmapScale != 1)
        {
            Run.Advance = 0;
            for (auto& Glyph : Run.Glyphs)
            {
                Glyph.Position.x_advance = static_cast<hb_position_t>(std::round(Glyph.Position.x_advance * Record.BitmapScale));
                Glyph.Position.y_advance = static_cast<hb_position_t>(std::round(Glyph.Position.y_advance * Record.BitmapScale));
                Glyph.Position.x_offset = static_cast<hb_position_t>(std::round(Glyph.Position.x_offset * Record.BitmapScale));
                Glyph.Position.y_offset = static_cast<hb_position_t>(std::round(Glyph.Position.y_offset * Record.BitmapScale));
                Run.Advance += static_cast<float>(Glyph.Position.x_advance) / 64.0f;
            }
        }
        if (Record.Fallbacks.empty()) return Run;
        std::set<std::uint32_t> Boundaries{End};
        for (const auto& Glyph : Run.Glyphs) Boundaries.insert(Glyph.Info.cluster);
        std::vector<ShapedGlyph> Combined;
        for (size_t Index = 0; Index < Run.Glyphs.size();)
        {
            const auto Cluster = Run.Glyphs[Index].Info.cluster;
            auto Last = Index + 1;
            while (Last < Run.Glyphs.size() && Run.Glyphs[Last].Info.cluster == Cluster) ++Last;
            bool Replaced = false;
            const bool Missing = std::any_of(Run.Glyphs.begin() + Index, Run.Glyphs.begin() + Last,
                [](const auto& Glyph) { return Glyph.Info.codepoint == 0; });
            if (Missing)
            {
                for (const auto Fallback : Record.Fallbacks)
                {
                    auto Replacement = shape(State->Fonts.at(Fallback.Value)->Font, IVisual.Text,
                        Cluster, *Boundaries.upper_bound(Cluster), &Run.Properties);
                    if (Replacement.Glyphs.empty() || std::any_of(Replacement.Glyphs.begin(), Replacement.Glyphs.end(),
                        [](const auto& Glyph) { return Glyph.Info.codepoint == 0; })) continue;
                    const auto Ratio = State->Fonts.at(Fallback.Value)->BitmapScale;
                    for (auto& Glyph : Replacement.Glyphs)
                    {
                        Glyph.Font = Fallback.Value;
                        Glyph.Position.x_advance = static_cast<hb_position_t>(std::round(Glyph.Position.x_advance * Ratio));
                        Glyph.Position.y_advance = static_cast<hb_position_t>(std::round(Glyph.Position.y_advance * Ratio));
                        Glyph.Position.x_offset = static_cast<hb_position_t>(std::round(Glyph.Position.x_offset * Ratio));
                        Glyph.Position.y_offset = static_cast<hb_position_t>(std::round(Glyph.Position.y_offset * Ratio));
                    }
                    Combined.insert(Combined.end(), Replacement.Glyphs.begin(), Replacement.Glyphs.end());
                    Replaced = true;
                    break;
                }
            }
            if (!Replaced) Combined.insert(Combined.end(), Run.Glyphs.begin() + Index, Run.Glyphs.begin() + Last);
            Index = Last;
        }
        Run.Glyphs = std::move(Combined);
        Run.Advance = 0;
        for (const auto& Glyph : Run.Glyphs) Run.Advance += static_cast<float>(Glyph.Position.x_advance) / 64.0f;
        return Run;
    };
    IVisual.Metrics.Ascent = static_cast<float>(Record.Face->size->metrics.ascender) * Record.BitmapScale / 64.0f / PixelScale;
    IVisual.Metrics.Descent = -static_cast<float>(Record.Face->size->metrics.descender) * Record.BitmapScale / 64.0f / PixelScale;
    IVisual.Metrics.LineHeight = std::max(IVisual.Metrics.Ascent + IVisual.Metrics.Descent,
        static_cast<float>(Record.Face->size->metrics.height) * Record.BitmapScale / 64.0f / PixelScale);
    for (const auto Fallback : Record.Fallbacks)
    {
        const auto& Face = *State->Fonts.at(Fallback.Value);
        IVisual.Metrics.Ascent = std::max(IVisual.Metrics.Ascent,
            Face.Face->size->metrics.ascender * Face.BitmapScale / 64.0f / PixelScale);
        IVisual.Metrics.Descent = std::max(IVisual.Metrics.Descent,
            -Face.Face->size->metrics.descender * Face.BitmapScale / 64.0f / PixelScale);
        IVisual.Metrics.LineHeight = std::max(IVisual.Metrics.LineHeight,
            Face.Face->size->metrics.height * Face.BitmapScale / 64.0f / PixelScale);
    }
    IVisual.Metrics.LineHeight = std::max(IVisual.Metrics.LineHeight, IVisual.Metrics.Ascent + IVisual.Metrics.Descent);
    if (!std::isfinite(IVisual.Metrics.LineHeight) || !std::isfinite(1.0f / PixelScale))
    {
        Error::raise(EErrorCode::InvalidArgument, "Scale produces unrepresentable logical metrics");
    }

    std::vector<std::pair<std::uint32_t, std::uint32_t>> Paragraphs;
    std::uint32_t Begin = 0;
    for (std::uint32_t Index = 0; Index < IVisual.Text.size(); ++Index)
    {
        const auto Point = IVisual.Text[Index];
        if (Point == U'\n' || Point == U'\r')
        {
            Paragraphs.emplace_back(Begin, Index);
            Begin = Index + 1;
            if (Point == U'\r' && Index + 1 < IVisual.Text.size() && IVisual.Text[Index + 1] == U'\n')
            {
                Begin = ++Index + 1;
            }
        }
    }
    Paragraphs.emplace_back(Begin, static_cast<std::uint32_t>(IVisual.Text.size()));
    for (const auto& [ParagraphBegin, ParagraphEnd] : Paragraphs)
    {
        const auto Paragraph = Shape(ParagraphBegin, ParagraphEnd);
        std::vector<std::uint32_t> Breaks;
        if (MaxWidth > 0)
        {
            std::set<std::uint32_t> ClusterBoundaries;
            for (const auto& Glyph : Paragraph.Glyphs)
            {
                ClusterBoundaries.insert(Glyph.Info.cluster);
            }
            for (auto Index = ParagraphBegin; Index + 1 < ParagraphEnd; ++Index)
            {
                if (isWrapSpace(IVisual.Text[Index]))
                {
                    if (ClusterBoundaries.contains(Index + 1))
                    {
                        Breaks.push_back(Index + 1);
                    }
                }
            }
        }
        Breaks.push_back(ParagraphEnd);
        auto LineBegin = ParagraphBegin;
        size_t BreakIndex = 0;
        do
        {
            auto LineEnd = Breaks[BreakIndex];
            auto Line = Shape(LineBegin, LineEnd, &Paragraph.Properties);
            auto ChosenIndex = BreakIndex;
            while (ChosenIndex + 1 < Breaks.size() && std::abs(Line.Advance) / PixelScale <= MaxWidth)
            {
                const auto CandidateEnd = Breaks[ChosenIndex + 1];
                auto Candidate = Shape(LineBegin, CandidateEnd, &Paragraph.Properties);
                if (std::abs(Candidate.Advance) / PixelScale > MaxWidth)
                {
                    break;
                }
                Line = std::move(Candidate);
                LineEnd = CandidateEnd;
                ++ChosenIndex;
            }
            State->appendLine(IVisual, Record, Font.Value, PixelSize, PixelScale, Line,
                              LineBegin, LineEnd, IVisual.Metrics.Height);
            LineBegin = LineEnd;
            BreakIndex = ChosenIndex + 1;
        } while (LineBegin < ParagraphEnd);
    }
    if (!std::isfinite(IVisual.Metrics.Width) || !std::isfinite(IVisual.Metrics.Height))
    {
        Error::raise(EErrorCode::InvalidArgument, "Text produces unrepresentable logical metrics");
    }
    return IVisual;
}
}