// 3D 网格片元着色器: 单方向光 + 环境项的简化光照, 让编辑器视口里的场景有立体感.

struct PSIn
{
    float4 Position : SV_Position;
    [[vk::location(0)]] float3 Normal : NORMAL;
    [[vk::location(1)]] float4 Color  : COLOR;
};

float4 main(PSIn Input) : SV_Target0
{
    const float3 LightDirection = normalize(float3(0.35, 0.80, 0.45));
    const float  Diffuse = saturate(dot(normalize(Input.Normal), LightDirection));
    const float3 Rgb = Input.Color.rgb * (0.25 + 0.75 * Diffuse);
    return float4(Rgb, Input.Color.a);
}
