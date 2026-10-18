#pragma once

#include <cstdint>

namespace ui
{
/**
 * @brief UI 失效原因，位标志可组合，语义参照 UE 的 EWidgetUpdateFlags：
 * 标记“哪些结果需要重算”，由帧循环消费后清除，而不是立即重算。
 */
enum class EInvalidationReason : std::uint32_t
{
    None = 0,
    // 尺寸或位置需要重算（measure + arrange）。隐含 Paint：顶点里烘焙了 Bounds 的平移量。
    IVisual = 1u << 0,
    // 绘制内容需要重新生成（顶点、索引、批次、裁剪）。
    Paint = 1u << 1,
    // 可见性变化，隐含 Paint 与 HitTest。
    Visibility = 1u << 2,
    // 孩子顺序变化，隐含 Paint 与 HitTest。
    ChildOrder = 1u << 3,
    // 命中测试相关状态变化（例如 AcceptEvents）。
    HitTest = 1u << 4,
    // 裁剪相关状态变化（例如 EnableOuterRender），隐含 Paint。
    Clipping = 1u << 5,
    All = IVisual | Paint | Visibility | ChildOrder | HitTest | Clipping
};

constexpr EInvalidationReason operator|(EInvalidationReason Left, EInvalidationReason Right) noexcept
{
    return static_cast<EInvalidationReason>(static_cast<std::uint32_t>(Left) | static_cast<std::uint32_t>(Right));
}

constexpr EInvalidationReason operator&(EInvalidationReason Left, EInvalidationReason Right) noexcept
{
    return static_cast<EInvalidationReason>(static_cast<std::uint32_t>(Left) & static_cast<std::uint32_t>(Right));
}

constexpr EInvalidationReason& operator|=(EInvalidationReason& Left, EInvalidationReason Right) noexcept
{
    Left = Left | Right;
    return Left;
}

constexpr EInvalidationReason operator~(EInvalidationReason Value) noexcept
{
    return static_cast<EInvalidationReason>(~static_cast<std::uint32_t>(Value)
        & static_cast<std::uint32_t>(EInvalidationReason::All));
}

// 查询 Reasons 是否包含 Test 中的任意一位。
constexpr bool hasInvalidation(EInvalidationReason Reasons, EInvalidationReason Test) noexcept
{
    return (static_cast<std::uint32_t>(Reasons) & static_cast<std::uint32_t>(Test)) != 0;
}

// 会改变绘制列表内容的原因集合；HitTest 不影响顶点，因此不在其中。
constexpr EInvalidationReason PaintInvalidationReasons =
    EInvalidationReason::Paint | EInvalidationReason::Visibility |
    EInvalidationReason::ChildOrder | EInvalidationReason::Clipping;

// 归一化：补齐隐含的从属原因。
constexpr EInvalidationReason normalizeInvalidation(EInvalidationReason Reason) noexcept
{
    if (hasInvalidation(Reason, EInvalidationReason::IVisual))
    {
        Reason |= EInvalidationReason::Paint;
    }
    if (hasInvalidation(Reason, EInvalidationReason::Visibility))
    {
        Reason |= EInvalidationReason::Paint | EInvalidationReason::HitTest;
    }
    if (hasInvalidation(Reason, EInvalidationReason::ChildOrder))
    {
        Reason |= EInvalidationReason::Paint | EInvalidationReason::HitTest;
    }
    if (hasInvalidation(Reason, EInvalidationReason::Clipping))
    {
        Reason |= EInvalidationReason::Paint;
    }
    return Reason;
}

// 一帧内因为失效实际做了哪些工作（保留模式下可能什么都没做）。
struct UIUpdateReport
{
    bool DidLayout{};
    bool DidPaint{};
    EInvalidationReason Reasons{EInvalidationReason::None};
};
}
