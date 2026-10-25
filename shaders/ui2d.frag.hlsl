// 2D UI 片元着色器: 采样图集(字体页/图片) 后乘以顶点色.
//
// 字体图集的像素是"白色 RGB + 覆盖率 alpha", 因此 albedo * tint 即可得到正确的
// 抗锯齿彩色文字; 纯色几何则绑定 1x1 白色纹理, 于是本管线可同时服务两者.
// 图集与图片都是直通 alpha, 与管线里 SrcAlpha/OneMinusSrcAlpha 的混合状态一致.

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]]
Texture2D<float4> Albedo : register(t0);

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]]
SamplerState AlbedoSampler : register(s0);

struct PSIn
{
    float4 Pos : SV_Position;
    [[vk::location(0)]] float2 UV   : TEXCOORD0;
    [[vk::location(1)]] float4 Tint : COLOR0;
};

float4 main(PSIn i) : SV_Target0
{
    return Albedo.Sample(AlbedoSampler, i.UV) * i.Tint;
}
