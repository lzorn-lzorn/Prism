// 3D 网格顶点着色器: 用 push constant 中的 MVP 变换到裁剪空间, 并把世界矩阵传给片元做法线变换.
//
// 顶点布局(与 SceneVertex 对齐, stride = 40):
//   location 0 : float3 Position  (偏移 0)
//   location 1 : float3 Normal    (偏移 12)
//   location 2 : float4 Color     (偏移 24)
//
// 注意: 这里刻意**不用** mul(Matrix, Vector). DXC 在 SPIR-V 后端会把
// mul(float4x4, float4) 降级为 OpVectorTimesMatrix(v * M), 与"行主序矩阵左乘列向量"
// 的数学语义相反, 会得到转置后的结果(表现就是场景被剧烈拉伸/撕裂).
// 因此把矩阵按"四个行向量"传入, 用显式 dot 自行完成左乘, 与 HLSL/CPU 的布局一致.

struct PushConstants
{
    // MVP 的四行(行主序).
    float4 Mvp0;
    float4 Mvp1;
    float4 Mvp2;
    float4 Mvp3;
    // World 的四行(行主序), 只取左上 3x3 变换法线.
    float4 World0;
    float4 World1;
    float4 World2;
    float4 World3;
};
[[vk::push_constant]] PushConstants Push;

struct VSIn
{
    [[vk::location(0)]] float3 Position : POSITION;
    [[vk::location(1)]] float3 Normal   : NORMAL;
    [[vk::location(2)]] float4 Color    : COLOR;
};

struct VSOut
{
    float4 Position : SV_Position;
    [[vk::location(0)]] float3 Normal : NORMAL;
    [[vk::location(1)]] float4 Color  : COLOR;
};

VSOut main(VSIn Input)
{
    const float4 P = float4(Input.Position, 1.0);

    VSOut Output;
    Output.Position = float4(
        dot(Push.Mvp0, P),
        dot(Push.Mvp1, P),
        dot(Push.Mvp2, P),
        dot(Push.Mvp3, P));
    Output.Normal = float3(
        dot(Push.World0.xyz, Input.Normal),
        dot(Push.World1.xyz, Input.Normal),
        dot(Push.World2.xyz, Input.Normal));
    Output.Color = Input.Color;
    return Output;
}
