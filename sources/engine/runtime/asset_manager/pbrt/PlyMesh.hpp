#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace runtime::asset
{

/**
 * @brief 从 Stanford PLY 加载出的三角网格(CPU 侧中间数据).
 *
 * 本结构是"资源加载层"的输出, 与 RHI/渲染器解耦: 调用方拿到后自行决定如何
 * 上传到 GPU(顶点布局、量化、LOD 分块都是渲染器的职责, 不在此处).
 *
 * 坐标/法线/UV 全部平铺存储:
 *   - Positions  : xyz, 共 VertexCount * 3 个 float;
 *   - Normals    : xyz, 共 VertexCount * 3 个 float(文件未提供时为空);
 *   - Texcoords  : uv,  共 VertexCount * 2 个 float(文件未提供时为空);
 *   - Indices    : 三角形, 共 TriangleCount * 3 个 uint32.
 */
struct PlyMesh
{
    std::vector<float> Positions;
    std::vector<float> Normals;
    std::vector<float> Texcoords;
    std::vector<std::uint32_t> Indices;

    std::uint32_t VertexCount { 0 };
    std::uint32_t TriangleCount { 0 };
    bool HasNormals { false };
    bool HasTexcoords { false };

    /** @brief 源文件路径(供缓存/调试), 内存加载时为空. */
    std::string Source;
};

/**
 * @brief Stanford PLY 加载器.
 *
 * 支持 ascii / binary_little_endian 两种格式(Kroken 全部为 binary_little_endian),
 * 顶点属性按名字匹配 x/y/z、nx/ny/nz、u/v(s/t), 面统一收敛为三角形
 * (非三角面做扇型三角化).
 */
class PlyMeshLoader
{
public:
    PlyMeshLoader() = delete;

    /** @brief 从磁盘加载 PLY; 失败(不存在/解析错/格式不支持)返回 std::nullopt. */
    static std::optional<PlyMesh> loadFromFile(const std::string& Path);

    /** @brief 从内存字节加载 PLY(语义同 loadFromFile). */
    static std::optional<PlyMesh> loadFromMemory(std::span<const std::byte> Data,
        const std::string& DebugName = {});
};

} // namespace runtime::asset
