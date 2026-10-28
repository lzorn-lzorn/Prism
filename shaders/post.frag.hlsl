// 后处理片元着色器: HDR 场景色 → 曝光 + ACES 近似 tonemap → gamma 编码.
// 后续可在本 Pass 前插入 Bloom(亮部阈值 + 分离模糊)与 TAA(历史缓冲 + 重投影).

Texture2D SceneColor : register(t0, space0);
SamplerState LinearSampler : register(s1, space0);

struct PushConstants
{
    float Exposure;   // 曝光
    float BloomStrength; // 预留
    float2 Pad;
};
[[vk::push_constant]] PushConstants Push;

struct PSIn
{
    float4 Position : SV_Position;
    [[vk::location(0)]] float2 UV : TEXCOORD0;
};

// ACES Filmic 近似(Fitted ACES), 输入输出均为线性 HDR.
float3 AcesToneMap(float3 X)
{
    const float A = 2.51;
    const float B = 0.03;
    const float C = 2.43;
    const float D = 0.59;
    const float E = 0.14;
    return saturate((X * (A * X + B)) / (X * (C * X + D) + E));
}

float4 main(PSIn Input) : SV_Target0
{
    float3 Color = SceneColor.SampleLevel(LinearSampler, Input.UV, 0).rgb;
    Color = AcesToneMap(Color * Push.Exposure);
    // sRGB 编码(gamma 近似 1/2.2); 若交换链为 sRGB 格式可关闭此段.
    Color = pow(Color, 1.0 / 2.2);
    return float4(Color, 1.0);
}
