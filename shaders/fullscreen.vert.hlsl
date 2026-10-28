// 全屏三角形顶点着色器: 用 SV_VertexID 生成覆盖整个视口的三角形(3 个顶点),
// 无需顶点缓冲. 供后处理(tonemap/bloom/TAA)等全屏 Pass 复用.

struct VSOut
{
    float4 Position : SV_Position;
    [[vk::location(0)]] float2 UV : TEXCOORD0;
};

VSOut main(uint VertexID : SV_VertexID)
{
    VSOut Output;
    // VertexID 0..2 -> 三角形 (-1,-1),(3,-1),(-1,3), 覆盖整个 NDC.
    const float2 UV = float2((VertexID << 1) & 2u, VertexID & 2u);
    Output.Position = float4(UV * 2.0 - 1.0, 0.0, 1.0);
    Output.UV = UV;
    return Output;
}
