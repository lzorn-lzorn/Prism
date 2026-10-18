[[vk::combinedImageSampler]] [[vk::binding(0, 2)]]
Texture2D<float4> Image : register(t0, space2);
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]]
SamplerState ImageSampler : register(s0, space2);

struct FragmentInput
{
    // Keep stage signatures aligned for DXIL: vertex output register 0 is
    // SV_Position, followed by UV/Tint. Vulkan locations remain explicit.
    float4 Position : SV_Position;
    [[vk::location(0)]] float2 UV : TEXCOORD0;
    [[vk::location(1)]] float4 Tint : TEXCOORD1;
};

float4 main(FragmentInput Input) : SV_Target0
{
    return Image.Sample(ImageSampler, Input.UV) * Input.Tint;
}