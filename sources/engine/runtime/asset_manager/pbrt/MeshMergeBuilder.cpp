#include "MeshMergeBuilder.hpp"

#include <cstddef>

namespace runtime::asset
{

MergedMesh MeshMergeBuilder::merge(const std::vector<MeshMergeInput>& Inputs)
{
    MergedMesh Out;

    bool AnyNormal = false;
    bool AnyTex = false;
    std::size_t TotalVerts = 0;
    std::size_t TotalTris = 0;
    for (const MeshMergeInput& In : Inputs)
    {
        TotalVerts += In.Positions.size() / 3;
        TotalTris += In.Indices.size() / 3;
        AnyNormal = AnyNormal || !In.Normals.empty();
        AnyTex = AnyTex || !In.Texcoords.empty();
    }
    Out.HasNormals = AnyNormal;
    Out.HasTexcoords = AnyTex;
    Out.Positions.reserve(TotalVerts * 3);
    if (AnyNormal)
        Out.Normals.reserve(TotalVerts * 3);
    if (AnyTex)
        Out.Texcoords.reserve(TotalVerts * 2);
    Out.Indices.reserve(TotalTris * 3);
    Out.SubMeshes.reserve(Inputs.size());

    std::uint32_t VertexBase = 0;
    for (const MeshMergeInput& In : Inputs)
    {
        const std::uint32_t N = static_cast<std::uint32_t>(In.Positions.size() / 3);
        const std::uint32_t Tri = static_cast<std::uint32_t>(In.Indices.size() / 3);

        Out.Positions.insert(Out.Positions.end(), In.Positions.begin(), In.Positions.end());

        // 法线/UV 缺失时按 0 补齐, 保证各子网格属性同构.
        if (AnyNormal)
        {
            if (In.Normals.size() >= static_cast<std::size_t>(N) * 3)
                Out.Normals.insert(Out.Normals.end(), In.Normals.begin(),
                    In.Normals.begin() + static_cast<std::ptrdiff_t>(N) * 3);
            else
                Out.Normals.insert(Out.Normals.end(), static_cast<std::size_t>(N) * 3, 0.0f);
        }
        if (AnyTex)
        {
            if (In.Texcoords.size() >= static_cast<std::size_t>(N) * 2)
                Out.Texcoords.insert(Out.Texcoords.end(), In.Texcoords.begin(),
                    In.Texcoords.begin() + static_cast<std::ptrdiff_t>(N) * 2);
            else
                Out.Texcoords.insert(Out.Texcoords.end(), static_cast<std::size_t>(N) * 2, 0.0f);
        }

        // 索引叠加顶点基址, 使子网格共享同一全局索引空间.
        const std::uint32_t FirstIndex = static_cast<std::uint32_t>(Out.Indices.size());
        for (std::uint32_t I = 0; I < Tri * 3; ++I)
            Out.Indices.push_back(In.Indices[I] + VertexBase);

        Out.SubMeshes.push_back(MergedSubMesh { VertexBase, FirstIndex, Tri * 3 });
        VertexBase += N;
    }

    Out.VertexCount = VertexBase;
    Out.TriangleCount = static_cast<std::uint32_t>(Out.Indices.size() / 3);
    return Out;
}

} // namespace runtime::asset
