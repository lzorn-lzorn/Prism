#pragma once

#include <concepts>
#include <type_traits>
namespace infra
{
// 通用 SMF 标签基类：仅存编译期布尔常量，不干涉任何特殊成员
template <bool CopyCtor, bool CopyAssign, bool MoveCtor, bool MoveAssign>
struct SMF_Control_Tag
{
    static constexpr bool enable_copy_ctor    = CopyCtor;
    static constexpr bool enable_copy_assign  = CopyAssign;
    static constexpr bool enable_move_ctor    = MoveCtor;
    static constexpr bool enable_move_assign  = MoveAssign;

    // 全部使用平凡默认成员，不删除, 不自定义，避免干扰类型特征
    SMF_Control_Tag() = default;
    ~SMF_Control_Tag() = default;
    SMF_Control_Tag(const SMF_Control_Tag&) = default;
    SMF_Control_Tag(SMF_Control_Tag&&) = default;
    SMF_Control_Tag& operator=(const SMF_Control_Tag&) = default;
    SMF_Control_Tag& operator=(SMF_Control_Tag&&) = default;
};

// 自动根据 Ty 推导四组开关：Ty 的属性决定 SMF 状态
template <typename Ty>
using SMF_AutoControl = SMF_Control_Tag<
    std::copy_constructible<Ty>,
    std::is_copy_assignable_v<Ty>,
    std::move_constructible<Ty>,
    std::is_move_assignable_v<Ty>
>;

}