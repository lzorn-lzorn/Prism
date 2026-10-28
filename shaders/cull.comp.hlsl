// GPU 视锥剔除计算着色器: 对实例包围球做 6 平面保守剔除, 把可见实例压缩后写回
// 间接绘制参数(DrawIndexedIndirect), 供 Opaque/Shadow Pass 以 vkCmdDrawIndexedIndirect 消费.
//
// 布局约定:
//   space0: 只读   CullParams(UBO, 6 平面 + 实例数) + InstanceBounds(SSBO)
//   space1: 读写   IndirectArgs(SSBO, 4×uint32: indexCount/instanceCount/firstIndex/vertexOffset)
//                  VisibleIndices(SSBO, 压缩后的可见实例下标)

cbuffer CullParams : register(b0, space0)
{
    float4 Plane0; // 左
    float4 Plane1; // 右
    float4 Plane2; // 下
    float4 Plane3; // 上
    float4 Plane4; // 远
    float4 Plane5; // 近
    uint   InstanceCount;
    uint3  CullPad;
};

StructuredBuffer<float4> InstanceBounds : register(t0, space0); // xyz = 中心, w = 半径

RWStructuredBuffer<uint>   IndirectArgs  : register(u0, space1); // [0]=indexCount [1]=instanceCount(计数器) [2]=firstIndex [3]=vertexOffset [4]=firstInstance
RWStructuredBuffer<uint>   VisibleIndices : register(u1, space1);

// 平面: Normal·p + Distance >= 0 为内侧(与 Renderer 的 Frustum 约定一致).
bool FrustumTest(float3 Center, float Radius)
{
    [unroll]
    for (int P = 0; P < 6; ++P)
    {
        float4 Plane = P == 0 ? Plane0 : (P == 1 ? Plane1 : (P == 2 ? Plane2 :
                       (P == 3 ? Plane3 : (P == 4 ? Plane4 : Plane5))));
        if (dot(Plane.xyz, Center) + Plane.w < -Radius)
            return false;
    }
    return true;
}

[numthreads(64, 1, 1)]
void main(uint3 DispatchThreadID : SV_DispatchThreadID)
{
    const uint Index = DispatchThreadID.x;
    if (Index >= InstanceCount)
        return;

    const float4 Bounds = InstanceBounds[Index];
    if (!FrustumTest(Bounds.xyz, Bounds.w))
        return;

    uint Slot = 0;
    InterlockedAdd(IndirectArgs[1], 1, Slot); // 以 instanceCount 字段作为原子计数器
    VisibleIndices[Slot] = Index;
}
