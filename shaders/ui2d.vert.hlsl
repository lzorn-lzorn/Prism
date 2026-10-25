// 2D UI 顶点着色器: 位置已由 CPU 侧变换为 NDC, 这里只做直通并转发 UV/顶点色.

struct VSIn
{
    [[vk::location(0)]] float2 Pos  : TEXCOORD0;
    [[vk::location(1)]] float2 UV   : TEXCOORD1;
    [[vk::location(2)]] float4 Tint : TEXCOORD2;
};

struct VSOut
{
    float4 Pos : SV_Position;
    [[vk::location(0)]] float2 UV   : TEXCOORD0;
    [[vk::location(1)]] float4 Tint : COLOR0;
};

VSOut main(VSIn i)
{
    VSOut o;
    o.Pos = float4(i.Pos, 0.0, 1.0);
    o.UV = i.UV;
    o.Tint = i.Tint;
    return o;
}
