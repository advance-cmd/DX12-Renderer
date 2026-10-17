// 天空球：跟着相机走，用局部顶点位置（转进天空图的坐标系）当查找向量去查立方体图（t2）
#include "LightingUtil.hlsl"   // PassConstants、gCubeMap、gsamLinearWrap

struct ObjectConstants
{
    float4x4 World;
    float4x4 WorldInvTranspose;
};
ConstantBuffer<ObjectConstants> gObj : register(b0);
ConstantBuffer<PassConstants> gPass : register(b1);

// 球的顶点格式和木箱一样（都是 Vertex），输入布局直接用木箱的
struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float3 PosL : POSITION;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    vout.PosL = SkyDir(vin.PosL); // 球没转，局部方向就是世界方向；转进天空图的坐标系就是查找向量
    float4 posW = mul(float4(vin.PosL, 1.0f), gObj.World);
    posW.xyz += gPass.EyePosW; // 球心永远在相机上，相机走不近也走不远
    vout.PosH = mul(posW, gPass.ViewProj).xyww; // 先变到裁剪空间，再把 z 换成 w：透视除法后深度恒为 1
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    // 面板"天空显示"：把天空换成 IBL 生成的图。查的方向一样，所以应该和原图对得上，只是更糊
    if (gPass.SkyView == 1)
        return float4(gIrradianceMap.SampleLevel(gsamLinearWrap, pin.PosL, 0).rgb, 1.0f);
    if (gPass.SkyView >= 2)
        return float4(gPrefilteredMap.SampleLevel(gsamLinearWrap, pin.PosL, gPass.SkyView - 2).rgb, 1.0f);
    return gCubeMap.Sample(gsamLinearWrap, pin.PosL);
}