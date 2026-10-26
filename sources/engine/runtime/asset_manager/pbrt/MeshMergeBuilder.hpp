#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace runtime::asset
{

/**
 * @brief 网格合并(CPU 侧): 把多份三角网格拼成"一个顶点区 + 一个索引区 + 子网格区间".
 *
 * 这是 GPU-driven / 合批绘制的**数据侧地基**: 合并后渲染器只需绑定一套顶点/索引缓冲,
 * 用每条子网格的 (FirstIndex, IndexCount, VertexOffset) 生成 indirect draw 参数,
 * 即可把 N 次 bind + draw 降为 1 次 bind + 1 次 MultiDrawIndirect.
 *
 * 注意: 本结构只负责"拼接顶点 + 重定位索引", 不涉及具体 GPU 顶点布局的交错/量化
 * (那是上传层的职责, 见 EVertexFormat 的压缩格式).
 */
struct MeshMergeInput
{
    std::span<const float> Positions;   // xyz * N(必填)
    std::span<const float> Normals;     // xyz * N(可空)
    std::span<const float> Texcoords;   // uv * N(可空)
    std::span<const std::uint32_t> Indices; // 三角形 * 3
};

/** @brief 合并后单条子网格的区间(顶点基址 + 索引区间). */
struct MergedSubMesh
{
    std::uint32_t VertexOffset { 0 };
    std::uint32_t IndexOffset { 0 };
    std::uint32_t IndexCount { 0 };
};

/** @brief 合并结果. */
struct MergedMesh
{
    std::vector<float> Positions;   // 拼接后的 xyz * 总顶点
    std::vector<float> Normals;     // 拼接后的 xyz * 总顶点(输入全缺省时为空)
    std::vector<float> Texcoords;   // 拼接后的 uv * 总顶点(输入全缺省时为空)
    std::vector<std::uint32_t> Indices; // 全局索引(已叠加各子网格顶点基址)
    std::vector<MergedSubMesh> SubMeshes;

    std::uint32_t VertexCount { 0 };
    std::uint32_t TriangleCount { 0 };
    bool HasNormals { false };
    bool HasTexcoords { false };
};

class MeshMergeBuilder
{
public:
    MeshMergeBuilder() = delete;

    /**
     * @brief 合并多份网格.
     * @note 所有输入的顶点属性必须同构(要么都有法线/UV, 要么都没有); 若只有部分输入
     *       带法线/UV, 缺失者按 0 补齐, 最终 HasNormals/HasTexcoords 取"至少一份有".
     */
    static MergedMesh merge(const std::vector<MeshMergeInput>& Inputs);
};

} // namespace runtime::asset
