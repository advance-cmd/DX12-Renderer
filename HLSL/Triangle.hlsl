// 带纹理的物体：VS 做局部 → 世界 → 裁剪空间，PS 采样漫反射纹理
// 根签名：b0 = 物体常量（根 CBV），t0 = 纹理（描述符表），b1 = Pass 常量（根 CBV），b2 = 材质，s0 = 静态采样器
// ConstantBuffer<T> 需要 Shader Model 5.1（vs_5_1 / ps_5_1）

#include "LightingUtil.hlsl"   // Light、MaterialConstants、MaxLights、ComputeDirectionalLight

// 必须和 C++ 里 FrameResource.h 的同名结构体一一对应
struct ObjectConstants
{
    float4x4 World;
    float4x4 WorldInvTranspose;
};

ConstantBuffer<ObjectConstants> gObj : register(b0);
ConstantBuffer<PassConstants> gPass : register(b1);
ConstantBuffer<MaterialConstants> gMat : register(b2);

Texture2D gDiffuseMap : register(t0);
SamplerState gsamLinearWrap : register(s0);

// 必须和 C++ 里的 Vertex 结构体、输入布局一致
struct VertexIn
{
    float3 PosL : POSITION; // L = Local，局部空间
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
};

struct VertexOut
{
    float4 PosH : SV_POSITION; // H = Homogeneous，齐次裁剪空间
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD;
    float3 PosW : TEXCOORD1;
    float4 shadowPosH : TEXCOORD2;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    float4 posW = mul(float4(vin.PosL, 1.0f), gObj.World); // 局部 → 世界
    vout.PosH = mul(posW, gPass.ViewProj); // 世界 → 裁剪空间
    vout.NormalW = mul(vin.NormalL, (float3x3)gObj.WorldInvTranspose); // 转到世界空间
    vout.TexC = vin.TexC;
    vout.PosW = posW.xyz;
    vout.shadowPosH = mul(posW,gPass.ShadowTransform);
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    float4 res=float4(0,0,0,0);
    float4 TexColor = gDiffuseMap.Sample(gsamLinearWrap, pin.TexC); // Sample 只能在像素着色器里用

    // 光照用的材质：反照率要带上贴图颜色，其余原样照抄 gMat
    MaterialConstants mat = gMat;
    mat.DiffuseAlbedo = TexColor * gMat.DiffuseAlbedo;
#ifdef ALPHA_TEST
    clip(mat.DiffuseAlbedo.a -0.1f);
#endif

    float3 ambient = gPass.AmbientLight.rgb * mat.DiffuseAlbedo.rgb;
    float3 toEye = gPass.EyePosW - pin.PosW;
    float distToEye = length(toEye);
    toEye /= distToEye;
    float3 normal = normalize(pin.NormalW);   // 插值之后长度不一定是 1
    float3 shadowFactor = float3(1.0f, 1.0f, 1.0f);
    shadowFactor[0] = CalcShadowFactor(pin.shadowPosH); // Lights[0] 是太阳，阴影图是站在太阳那里画的
    float3 direct = ComputeLighting(gPass.Lights, mat, pin.PosW, normal, toEye,shadowFactor);
    res = float4(ambient + direct, mat.DiffuseAlbedo.a);
#ifdef FOG
    res.rgb = ApplyFog(res.rgb, distToEye, gPass.FogColor, gPass.FogStart, gPass.FogRange);
#endif
    return res;
}

VertexOut VS_MirrorClear(VertexIn vin)
{
    VertexOut vout = VS(vin);            // 位置照常算
    vout.PosH.z = vout.PosH.w * 0.9999f; // 透视除法后 z/w ≈ 1，也就是最远处
    return vout;
}

float4 PS_MirrorClear(VertexOut pin) : SV_Target
{
    return gPass.FogColor;
}

// 阴影 Pass：只要深度。铁丝网的洞要 clip 掉，不然影子是一整块
void PS_Shadow(VertexOut pin)
{
    float4 TexColor = gDiffuseMap.Sample(gsamLinearWrap, pin.TexC); // Sample 只能在像素着色器里用
    clip(TexColor.a * gMat.DiffuseAlbedo.a - 0.1f);
}
