#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

/**
 * ============================================================================
 *  infra::hash —— 通用散列基础设施
 * ============================================================================
 *
 *  [要解决的问题]
 *  渲染器在多个子系统里反复需要"把若干异构字段(整数 / 枚举 / 浮点 / 字符串 / 字节块)
 *  折合成一个确定性的 64 位键", 用作语义缓存键(pipeline 键、BindGroup 键、renderpass 键、
 *  着色器资源 id 等)。历史上这段逻辑被复制粘贴了多处, 且算法不统一(FNV-1a 与
 *  boost::hash_combine 混用), 既难维护也难保证一致。
 *
 *  本文件把"散列"这一与具体场景无关的底层操作收敛成一组纯函数, 提供两种互补的算法:
 *
 *      - FNV-1a          : 按字节流散列, 分布好、结果稳定、适合"内容 → 摘要"的场景
 *                         (着色器源码、资源名、blob)。
 *      - hash_combine    : 常数时间把单个标量值混合进种子, 适合"结构体逐字段折合"的
 *                         场景(pipeline 描述符等)。
 *
 *  [实现要点]
 *      - FNV-1a 64 位: offset basis = 14695981039346656037, prime = 1099511628211。
 *      - hash_combine 采用 boost 的黄金比例常数 0x9e3779b97f4a7c15。
 *      - 全部 constexpr, 无状态、无分配、无异常, 可直接在编译期与热路径使用。
 *
 *  [命名约定]
 *      - hashBytes / hashValue : FNV-1a, 按引用就地累加种子(与 boost::hash_combine 一致)。
 *      - hashCombine           : 混合算法, 按引用就地累加种子。
 *      - hashString            : FNV-1a 的一次性便捷入口(返回结果)。
 *
 *  [线程 / 生命周期]
 *      纯函数, 无共享状态; 任意线程、任意时刻可安全调用。
 */

namespace infra
{

// ============================================================================
// 常量
// ============================================================================

/** @brief FNV-1a 64 位偏移基(offset basis)。 */
inline constexpr uint64_t Fnv1aOffsetBasis64 = 14695981039346656037ull;

/** @brief FNV-1a 64 位素数。 */
inline constexpr uint64_t Fnv1aPrime64 = 1099511628211ull;

/** @brief boost::hash_combine 使用的黄金比例常数。 */
inline constexpr uint64_t HashCombineMagic = 0x9e3779b97f4a7c15ull;

// ============================================================================
// FNV-1a
// ============================================================================

/**
 * @brief 对一段字节做 FNV-1a 64 位散列, 就地累加进种子。
 * @param Seed 输入输出种子; 首次调用通常传入 Fnv1aOffsetBasis64。
 * @param Bytes 待散列的字节范围。
 * @note 该函数按字节顺序敏感地处理数据, 适合"内容 → 摘要"; 不适合需要顺序无关
 *       或对整数做快速混合的场景(见 hashCombine)。
 */
constexpr void hashBytes(uint64_t& Seed, std::span<const std::byte> Bytes) noexcept
{
    for (const std::byte Byte : Bytes)
    {
        Seed ^= static_cast<uint64_t>(std::to_integer<uint8_t>(Byte));
        Seed *= Fnv1aPrime64;
    }
}

/**
 * @brief 对 UTF-8 字符串视图做 FNV-1a 散列, 返回一次性结果。
 * @param Text 字符串内容。
 * @param Seed 起始种子, 缺省为 Fnv1aOffsetBasis64。
 * @return 散列结果。
 */
[[nodiscard]] constexpr uint64_t hashString(
    std::string_view Text,
    uint64_t Seed = Fnv1aOffsetBasis64) noexcept
{
    for (const char Ch : Text)
    {
        Seed ^= static_cast<uint64_t>(static_cast<uint8_t>(Ch));
        Seed *= Fnv1aPrime64;
    }
    return Seed;
}

/**
 * @brief 对平凡可复制对象按内存字节做 FNV-1a 散列, 就地累加进种子。
 * @tparam T 平凡可复制类型。
 * @param Seed 输入输出种子。
 * @param Value 待散列对象。
 * @note 对象含填充字节时会把填充一并散列, 而填充值未定义 → 结果不确定。对含填充的
 *       结构体应改用 hashCombine 逐字段散列, 而不是直接 hashValue 整个结构体。
 */
template <typename T>
    requires std::is_trivially_copyable_v<T>
constexpr void hashValue(uint64_t& Seed, const T& Value) noexcept
{
    hashBytes(Seed, std::as_bytes(std::span { &Value, 1 }));
}

// ============================================================================
// hash_combine
// ============================================================================

/**
 * @brief boost 风格的增量散列混合, 把单个 64 位值折合进种子。
 * @param Seed 输入输出种子。
 * @param Value 待混合值。
 * @note 混合操作常数时间、可逆性弱, 只用于"同一实体逐字段累加"的缓存键构造;
 *       不要用它做内容摘要或安全散列。
 */
constexpr void hashCombine(uint64_t& Seed, uint64_t Value) noexcept
{
    Seed ^= Value + HashCombineMagic + (Seed << 6) + (Seed >> 2);
}

/**
 * @brief 把整数 / 枚举 / 浮点标量折合进种子。
 * @tparam T 整数、枚举或浮点类型。
 * @param Seed 输入输出种子。
 * @param Value 待混合值。
 * @note 浮点按位模式混合(而非数值截断), 保证 3.14 与 3.15 不塌缩成同一键。
 */
template <typename T>
    requires (std::is_enum_v<T> || std::integral<T> || std::floating_point<T>)
constexpr void hashCombine(uint64_t& Seed, T Value) noexcept
{
    if constexpr (std::floating_point<T>)
    {
        if constexpr (sizeof(T) == sizeof(uint32_t))
        {
            hashCombine(Seed, static_cast<uint64_t>(std::bit_cast<uint32_t>(Value)));
        }
        else
        {
            hashCombine(Seed, std::bit_cast<uint64_t>(Value));
        }
    }
    else
    {
        hashCombine(Seed, static_cast<uint64_t>(Value));
    }
}

// ============================================================================
// 便捷入口
// ============================================================================

/**
 * @brief 从固定种子开始, 依次把多个值折合成一个 64 位键。
 * @tparam Ts 各值类型(整数 / 枚举 / 浮点)。
 * @param Values 待混合的值序列。
 * @return 折合结果。
 * @note 顺序敏感; 等值序列在不同类型组合下可能碰撞, 需要强唯一性时应自行加类型标签。
 */
template <typename... Ts>
[[nodiscard]] constexpr uint64_t hashCombineMany(const Ts&... Values) noexcept
{
    uint64_t Seed = Fnv1aOffsetBasis64;
    (hashCombine(Seed, Values), ...);
    return Seed;
}

} // namespace infra
