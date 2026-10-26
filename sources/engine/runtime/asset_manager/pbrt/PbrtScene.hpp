#pragma once

#include <infra/math/Matrix.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace runtime::asset::pbrt
{

/**
 * @brief 光谱/颜色值: 既可以是常量 RGB, 也可以是纹理引用.
 *
 * pbrt 的 "rgb reflectance" / "texture reflectance" / "spectrum eta" 等参数最终
 * 都归结为"要么一个 RGB 常量, 要么一张(或一个命名)纹理". 加载层先按此抽象落盘,
 * 具体求值留给材质系统.
 */
struct Spectrum
{
    std::array<float, 3> RGB { 0.0f, 0.0f, 0.0f };
    std::string Texture;
    bool IsTexture { false };
};

/**
 * @brief 材质(解析后的精简视图).
 *
 * 只提取"第一版 PBR 材质"需要的字段; 其余参数原样存入 Extra, 避免丢信息,
 * 后续(coat 层、Fresnel 曲线、厚度图等)可按需扩展.
 */
struct Material
{
    std::string Type; // diffuse / coateddiffuse / conductor / coatedconductor / dielectric / ...
    std::optional<Spectrum> Reflectance;      // albedo / reflectance
    std::optional<Spectrum> Roughness;        // 标量粗糙度(可为纹理)
    std::optional<std::string> DisplacementMap;
    std::optional<std::string> NormalMap;
    float Ior { 1.5f };
    std::map<std::string, std::string> Extra; // 其余参数(type -> 原始字符串)
};

/** @brief 一个网格源: plymesh 文件, 或内联 trianglemesh 的三角汤. */
struct MeshRef
{
    std::string File;                        // 为空表示内联 trianglemesh
    std::vector<float> Positions;            // xyz * N
    std::vector<float> Normals;              // xyz * N(可空)
    std::vector<float> Texcoords;            // uv * N(可空)
    std::vector<std::uint32_t> Indices;      // 三角形 * 3
};

/** @brief 一个网格实例: 网格 + 世界变换 + 材质. */
struct Instance
{
    std::uint32_t MeshId { 0 };
    std::uint32_t MaterialId { 0 };
    infra::Matrix4x4 Transform = infra::Matrix4x4::Identity();
};

/** @brief 光源(解析后的精简视图). */
struct Light
{
    enum class Kind
    {
        Infinite, // 环境光(可带环境贴图/portal)
        Area,     // 面光源
        Point,
        Spot,
        Distant,
    };

    Kind K { Kind::Infinite };
    std::array<float, 3> Color { 1.0f, 1.0f, 1.0f };
    float Scale { 1.0f };
    infra::Matrix4x4 Transform = infra::Matrix4x4::Identity();
    std::optional<std::string> MeshFile; // 网格面光源引用的 PLY
};

/** @brief 相机(perspective). */
struct Camera
{
    std::array<float, 3> Eye { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> LookAt { 0.0f, 0.0f, -1.0f };
    std::array<float, 3> Up { 0.0f, 1.0f, 0.0f };
    float Fov { 17.0f };
};

/**
 * @brief pbrt 场景解析结果: 加载层的最终输出(与 RHI/渲染器解耦).
 */
struct Scene
{
    std::vector<MeshRef> Meshes;
    std::vector<Instance> Instances;
    std::vector<Material> Materials; // [0] 固定为默认材质
    std::vector<Light> Lights;
    std::optional<Camera> Cam;
};

/** @brief 解析结果 + 非致命告警(未知指令/未解析参数等). */
struct ParseResult
{
    Scene Scene;
    std::vector<std::string> Warnings;
};

/**
 * @brief 解析一个 pbrt 场景文件(递归解析 Include).
 *
 * @param EntryPath 入口 .pbrt 文件的绝对/相对路径; Include 相对其所在目录解析.
 * @return 解析结果; 严重错误(文件不存在等)会抛出 std::runtime_error,
 *         非致命问题(未知指令/未知材质参数)记入 Warnings.
 *
 * @note 本函数只做"场景图 + 元数据"解析, **不会读取 PLY 网格字节**; 网格数据由
 *       PlyMeshLoader 按需懒加载.
 */
ParseResult parseSceneFile(const std::string& EntryPath);

} // namespace runtime::asset::pbrt
