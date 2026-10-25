// 三角形演示着色器: 片元阶段。

struct PSInput
{
    float4 Position : SV_Position;
    [[vk::location(0)]] float3 Color : COLOR0;
};

float4 main(PSInput Input) : SV_Target0
{
    return float4(Input.Color, 1.0);
}
