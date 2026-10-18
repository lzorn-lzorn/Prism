[[vk::binding(0, 1)]]
cbuffer Frame : register(b0, space1)
{
    float4 Transform;
};

struct VertexInput
{
    [[vk::location(0)]] float2 Position : TEXCOORD0;
    [[vk::location(1)]] float2 UV : TEXCOORD1;
    [[vk::location(2)]] float4 Tint : TEXCOORD2;
};

struct VertexOutput
{
    float4 Position : SV_Position;
    [[vk::location(0)]] float2 UV : TEXCOORD0;
    [[vk::location(1)]] float4 Tint : TEXCOORD1;
};

VertexOutput main(VertexInput Input)
{
    VertexOutput Output;
    Output.Position = float4(Input.Position * Transform.xy + Transform.zw, 0.0, 1.0);
    Output.UV = Input.UV;
    Output.Tint = Input.Tint;
    return Output;
}