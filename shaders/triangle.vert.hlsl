// 三角形演示着色器: 顶点阶段。
// 约定与引擎 RHI 的 VertexInputState 对齐:
//   location 0 = float3 Position
//   location 1 = float3 Color
// 变换使用 cbuffer Frame(ViewProjection), 由渲染器每帧上传。

cbuffer Frame : register(b0, space0)
{
    float4x4 ViewProjection;
};

struct VSInput
{
    [[vk::location(0)]] float3 Position : POSITION0;
    [[vk::location(1)]] float3 Color    : COLOR0;
};

struct VSOutput
{
    float4 Position : SV_Position;
    [[vk::location(0)]] float3 Color : COLOR0;
};

VSOutput main(VSInput Input)
{
    VSOutput Output;
    Output.Position = mul(ViewProjection, float4(Input.Position, 1.0));
    Output.Color = Input.Color;
    return Output;
}
