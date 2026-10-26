/**
 * ============================================================================
 *  tests/pbrt_load.cpp
 * ============================================================================
 *
 *  资源加载层(pbrt 场景 + PLY 网格)的 CPU 侧验证.
 *
 *  覆盖:
 *      1. PLY 二进制加载器(顶点/法线/UV/索引的解析与计数)
 *      2. pbrt 场景解析: Shape/NamedMaterial/Transform 栈/Include/Object 实例化/
 *         光源/相机
 *      3. pbrt 行向量矩阵 → 引擎列向量矩阵的转置约定(用已知 Transform 校验)
 *
 *  全部不依赖 GPU. 未找到 Kroken 资产目录时打印 SKIP 并以 0 退出.
 *  用法: pbrt_load [assets/kroken 目录]  (默认 ../assets/kroken)
 * ============================================================================
 */

#include <PlyMesh.hpp>
#include <PbrtScene.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

namespace
{

int g_checks = 0;
int g_failures = 0;

#define CHECK(Expr)                                                     \
    do                                                                  \
    {                                                                   \
        ++g_checks;                                                     \
        if (!(Expr))                                                    \
        {                                                               \
            std::printf("    [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #Expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (false)

bool fileExists(const std::string& Path)
{
    std::ifstream File(Path);
    return File.good();
}

} // namespace

int main(int Argc, char** Argv)
{
    const std::string SceneDir = Argc > 1 ? Argv[1] : "../assets/kroken";
    const std::string Entry = SceneDir + "/camera-1.pbrt";
    if (!fileExists(Entry))
    {
        std::printf("pbrt asset load tests SKIPPED (scene dir not found: %s)\n", SceneDir.c_str());
        return 0;
    }

    // ---- 1. PLY 二进制加载器(mesh_00001.ply: 400 顶点 / 200 面) ----
    {
        const auto Mesh = runtime::asset::PlyMeshLoader::loadFromFile(SceneDir + "/geometry/mesh_00001.ply");
        CHECK(Mesh.has_value());
        if (Mesh)
        {
            CHECK(Mesh->VertexCount == 400);
            CHECK(Mesh->TriangleCount == 200);
            CHECK(Mesh->HasNormals);
            CHECK(Mesh->HasTexcoords);
            CHECK(Mesh->Positions.size() == 400 * 3);
            CHECK(Mesh->Normals.size() == 400 * 3);
            CHECK(Mesh->Texcoords.size() == 400 * 2);
            CHECK(Mesh->Indices.size() == 200 * 3);
            // 第一个三角形索引应是合法的 0/1/2(不同顶点).
            CHECK(Mesh->Indices[0] != Mesh->Indices[1]);
            CHECK(Mesh->Indices[1] != Mesh->Indices[2]);
        }
    }

    // ---- 2. pbrt 场景解析 ----
    auto Result = runtime::asset::pbrt::parseSceneFile(Entry);
    const auto& Scene = Result.Scene;

    std::printf("  meshes=%zu instances=%zu materials=%zu lights=%zu warnings=%zu\n",
        Scene.Meshes.size(), Scene.Instances.size(), Scene.Materials.size(),
        Scene.Lights.size(), Result.Warnings.size());

    CHECK(Scene.Meshes.size() >= 240);
    CHECK(Scene.Instances.size() >= 240);
    CHECK(Scene.Meshes.size() == Scene.Instances.size()); // 每个网格恰好实例化一次
    CHECK(Scene.Materials.size() >= 60);
    CHECK(Scene.Lights.size() == 3);
    CHECK(Scene.Cam.has_value());
    if (Scene.Cam)
        CHECK(std::fabs(Scene.Cam->Fov - 17.0) < 0.001);

    // 全部实例引用合法的网格/材质.
    bool ReferencesValid = true;
    for (const auto& Inst : Scene.Instances)
    {
        if (Inst.MeshId >= Scene.Meshes.size() || Inst.MaterialId >= Scene.Materials.size())
        {
            ReferencesValid = false;
            break;
        }
    }
    CHECK(ReferencesValid);

    // 光源: 2 个面光源 + 1 个无限光.
    int AreaCount = 0;
    int InfiniteCount = 0;
    for (const auto& Light : Scene.Lights)
    {
        if (Light.K == runtime::asset::pbrt::Light::Kind::Area)
            ++AreaCount;
        if (Light.K == runtime::asset::pbrt::Light::Kind::Infinite)
            ++InfiniteCount;
    }
    CHECK(AreaCount == 2);
    CHECK(InfiniteCount == 1);

    // 网格面光源应回填出文件名(mesh_00153.ply).
    bool HasMeshAreaLight = false;
    for (const auto& Light : Scene.Lights)
    {
        if (Light.K == runtime::asset::pbrt::Light::Kind::Area && Light.MeshFile)
        {
            CHECK(*Light.MeshFile == "geometry/mesh_00153.ply");
            HasMeshAreaLight = true;
        }
    }
    CHECK(HasMeshAreaLight);

    // ---- 3. 矩阵约定校验: mesh_00128 的世界平移 = (259.31, 106.908, -480.425) ----
    {
        bool Found = false;
        for (std::size_t I = 0; I < Scene.Meshes.size(); ++I)
        {
            if (Scene.Meshes[I].File == "geometry/mesh_00128.ply")
            {
                for (const auto& Inst : Scene.Instances)
                {
                    if (Inst.MeshId == I)
                    {
                        const auto& M = Inst.Transform;
                        CHECK(std::fabs(M[0][3] - 259.31f) < 0.01f);
                        CHECK(std::fabs(M[1][3] - 106.908f) < 0.01f);
                        CHECK(std::fabs(M[2][3] + 480.425f) < 0.01f);
                        Found = true;
                    }
                }
            }
        }
        CHECK(Found);
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0)
    {
        std::printf("pbrt asset load tests FAILED\n");
        return 1;
    }
    std::printf("pbrt asset load tests passed.\n");
    return 0;
}
