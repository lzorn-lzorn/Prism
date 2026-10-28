// 3D 网格顶点着色器(PBR): 用 push constant 中的 MVP 变换到裁剪空间, 并输出世界空间
// 位置与法线供片元阶段做 GGX 光照与阴影采样.
//
// 顶点布局(与 SceneVertex 对齐, stride = 40):
//   location 0 : float3 Position  (偏移 0)
//   location 1 : float3 Normal    (偏移 12)
//   location 2 : float4 Color     (偏移 24, 用作 base color / 材质近似)
//
// 注意: 这里刻意**不用** mul(Matrix, Vector). DXC 在 SPIR-V 后端会把
// mul(float4x4, float4) 降级为 OpVectorTimesMatrix(v * M), 与"行主序矩阵左乘列向量"
// 的数学语义相反. 因此把矩阵按"四个行向量"传入, 用显式 dot 自行完成左乘.

struct PushConstants
{
    // MVP 的四行(行主序).
    float4 Mvp0;
    float4 Mvp1;
    float4 Mvp2;
    float4 Mvp3;
    // World 的四行(行主序), 左上 3x3 用于变换法线, 全 4x4 用于世界坐标.
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
    [[vk::location(0)]] float3 WorldPosition : TEXCOORD0;
    [[vk::location(1)]] float3 WorldNormal   : TEXCOORD1;
    [[vk::location(2)]] float4 Color         : TEXCOORD2;
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
    Output.WorldPosition = float3(
        dot(Push.World0, P),
        dot(Push.World1, P),
        dot(Push.World2, P),
        dot(Push.World3, P));
    Output.WorldNormal = float3(
        dot(Push.World0.xyz, Input.Normal),
        dot(Push.World1.xyz, Input.Normal),
        dot(Push.World2.xyz, Input.Normal));
    Output.Color = Input.Color;
    return Output;
}
