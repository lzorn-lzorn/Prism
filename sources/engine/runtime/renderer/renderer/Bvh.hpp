#pragma once

#include <renderer/Culling.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace runtime::renderer
{

/**
 * @brief 包围盒层次(BVH): 对一组 AABB 做中位切分, 提供视锥查询.
 *
 * 用途: 当场景代理数量庞大(数十万/百万级)时, 用 BVH 把"逐代理视锥测试"从 O(N)
 * 降到 O(log N). Kroken 的代理只有几百个, 但按"网格分块"拆分后(一个大网格切块),
 * 分块数量会迅速膨胀, 此时 BVH 是必要的前置结构.
 *
 * 实现选择"中位切分"(按最长轴质心排序 + nth_element), 无需 SAH/莫顿码, 简单且
 * 对静态场景足够; 叶节点包含不超过 LeafSize 个图元.
 */
class Bvh
{
public:
    struct Node
    {
        Aabb Bounds;
        // 叶节点: 覆盖 PrimIndices_[Left, Right); 内部节点: 左右子节点在 Nodes_ 中的下标.
        std::uint32_t Left { 0 };
        std::uint32_t Right { 0 };
        bool IsLeaf { true };
    };

    /** @brief 用给定 AABB 集合重建 BVH(Index 即原始下标). */
    void build(std::span<const Aabb> Bounds)
    {
        Nodes_.clear();
        PrimIndices_.clear();
        if (Bounds.empty())
            return;

        PrimIndices_.resize(Bounds.size());
        for (std::size_t I = 0; I < Bounds.size(); ++I)
            PrimIndices_[I] = static_cast<std::uint32_t>(I);

        // 内部节点先占位(最坏 2N), 根节点下标恒为 0(buildNode 顺序分配).
        Nodes_.reserve(Bounds.size() * 2);
        (void)buildNode(Bounds, 0, static_cast<std::uint32_t>(Bounds.size()));
    }

    /** @brief 查询与视锥相交的原始下标(输出为原始 AABB 数组的下标). */
    void query(const Frustum& F, std::span<const Aabb> Bounds, std::vector<std::uint32_t>& Out) const
    {
        Out.clear();
        if (Nodes_.empty())
            return;
        queryNode(F, Bounds, 0, Out);
    }

    [[nodiscard]] const std::vector<Node>& nodes() const noexcept { return Nodes_; }
    [[nodiscard]] std::size_t nodeCount() const noexcept { return Nodes_.size(); }

private:
    static constexpr std::uint32_t LeafSize = 4;

    std::uint32_t buildNode(std::span<const Aabb> Bounds, std::uint32_t Begin, std::uint32_t End)
    {
        // 计算 [Begin, End) 的合并 AABB.
        Aabb Box = Bounds[PrimIndices_[Begin]];
        for (std::uint32_t I = Begin + 1; I < End; ++I)
            Box.merge(Bounds[PrimIndices_[I]]);

        if (End - Begin <= LeafSize)
        {
            Nodes_.push_back(Node { Box, Begin, End, true });
            return static_cast<std::uint32_t>(Nodes_.size() - 1);
        }

        // 选最长轴, 按质心做中位切分.
        const infra::Vec3f Extent = Box.Max - Box.Min;
        int Axis = 0;
        if (Extent[1] > Extent[Axis])
            Axis = 1;
        if (Extent[2] > Extent[Axis])
            Axis = 2;

        const std::uint32_t Mid = Begin + (End - Begin) / 2;
        const std::span<const Aabb> B = Bounds;
        const int A = Axis;
        std::nth_element(PrimIndices_.begin() + Begin, PrimIndices_.begin() + Mid,
            PrimIndices_.begin() + End,
            [&](std::uint32_t Lhs, std::uint32_t Rhs)
            {
                return B[Lhs].center()[A] < B[Rhs].center()[A];
            });

        const std::uint32_t NodeIndex = static_cast<std::uint32_t>(Nodes_.size());
        Nodes_.push_back(Node { Box, 0, 0, false }); // 占位, 随后回填
        const std::uint32_t LeftChild = buildNode(Bounds, Begin, Mid);
        const std::uint32_t RightChild = buildNode(Bounds, Mid, End);
        Nodes_[NodeIndex].Left = LeftChild;
        Nodes_[NodeIndex].Right = RightChild;
        Nodes_[NodeIndex].IsLeaf = false;
        return NodeIndex;
    }

    void queryNode(const Frustum& F, std::span<const Aabb> Bounds, std::uint32_t NodeIndex,
        std::vector<std::uint32_t>& Out) const
    {
        const Node& N = Nodes_[NodeIndex];
        if (!contains(F, N.Bounds))
            return;
        if (N.IsLeaf)
        {
            for (std::uint32_t I = N.Left; I < N.Right; ++I)
            {
                const std::uint32_t Prim = PrimIndices_[I];
                if (contains(F, Bounds[Prim]))
                    Out.push_back(Prim);
            }
            return;
        }
        queryNode(F, Bounds, N.Left, Out);
        queryNode(F, Bounds, N.Right, Out);
    }

    std::vector<Node> Nodes_;
    std::vector<std::uint32_t> PrimIndices_;
};

} // namespace runtime::renderer
