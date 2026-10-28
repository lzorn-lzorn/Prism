// 3D 网格片元着色器: PBR(GGX 高光 + Lambert 漫反射) + 阴影贴图采样.
//
// 帧数据(Set 0)由渲染器每帧上传; 阴影贴图使用比较采样器(PCF 可后续扩展).

cbuffer FrameData : register(b0, space0)
{
    float4 CameraPosition;    // xyz = 相机位置, w 未用
    float4 LightDirection;    // xyz = 指向光源的方向(归一化), w 未用
    float4 LightColor;        // rgb = 光源颜色, w = 强度
    float4 ShadowMatrix0;     // 阴影 ViewProjection 行 0(行主序)
    float4 ShadowMatrix1;     // 行 1
    float4 ShadowMatrix2;     // 行 2
    float4 ShadowMatrix3;     // 行 3
    float4 ShadowParams;      // x = 深度 bias, y = 阴影贴图尺寸, z = 阴影开关, w = 环境项强度
};

Texture2D ShadowMap : register(t1, space0);
SamplerComparisonState ShadowSampler : register(s2, space0);

struct PSIn
{
    float4 Position : SV_Position;
    [[vk::location(0)]] float3 WorldPosition : TEXCOORD0;
    [[vk::location(1)]] float3 WorldNormal   : TEXCOORD1;
    [[vk::location(2)]] float4 Color         : TEXCOORD2;
};

float4 main(PSIn Input) : SV_Target0
{
    const float3 N = normalize(Input.WorldNormal);
    const float3 V = normalize(CameraPosition.xyz - Input.WorldPosition);
    const float3 L = normalize(LightDirection.xyz);
    const float3 H = normalize(V + L);

    // 编辑器预览材质: base color 取顶点色; 粗糙度由 alpha 近似.
    const float3 Albedo = Input.Color.rgb;
    const float Metallic = 0.0;
    const float Roughness = clamp(Input.Color.a * 0.5 + 0.3, 0.05, 1.0);

    // ---- GGX 分布 / Smith 几何 / Schlick 菲涅尔 ----
    const float NdotH = saturate(dot(N, H));
    const float NdotL = saturate(dot(N, L));
    const float NdotV = saturate(dot(N, V));
    const float a = Roughness * Roughness;
    const float a2 = a * a;
    const float Denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    const float D = a2 / (3.14159265359 * Denom * Denom);
    const float k = Roughness * 0.5;
    const float G = (NdotL / (NdotL * (1.0 - k) + k)) * (NdotV / (NdotV * (1.0 - k) + k));
    const float F0 = 0.04;
    const float3 F = F0 + (1.0 - F0) * pow(1.0 - saturate(dot(V, H)), 5.0);

    const float3 Specular = D * G * F / max(4.0 * NdotV * NdotL, 0.001);
    const float3 Diffuse = Albedo * (1.0 - Metallic) / 3.14159265359;
    const float3 Direct = (Diffuse + Specular) * LightColor.rgb * LightColor.w * NdotL;

    // ---- 阴影贴图采样(单 tap, PCF 留待后续) ----
    float Shadow = 1.0;
    if (ShadowParams.z > 0.5)
    {
        const float4 WorldPos = float4(Input.WorldPosition, 1.0);
        const float4 ShadowClip = float4(
            dot(ShadowMatrix0, WorldPos),
            dot(ShadowMatrix1, WorldPos),
            dot(ShadowMatrix2, WorldPos),
            dot(ShadowMatrix3, WorldPos));
        float3 ShadowUV = ShadowClip.xyz / ShadowClip.w;
        ShadowUV = ShadowUV * 0.5 + 0.5;
        ShadowUV.y = 1.0 - ShadowUV.y; // Vulkan NDC y 向下 → UV y 向上
        if (all(ShadowUV.xy >= 0.0) && all(ShadowUV.xy <= 1.0))
        {
            Shadow = ShadowMap.SampleCmpLevelZero(
                ShadowSampler, ShadowUV.xy, ShadowUV.z - ShadowParams.x);
        }
    }

    const float3 Color = Direct * Shadow + Albedo * ShadowParams.w;
    return float4(Color, 1.0);
}
