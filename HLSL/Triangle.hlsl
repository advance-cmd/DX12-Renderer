// 带纹理的物体：VS 做局部 → 世界 → 裁剪空间，PS 采样漫反射纹理
// 根签名：b0 = 物体常量（根 CBV），t0 = 纹理（描述符表），b1 = Pass 常量（根 CBV），b2 = 材质，s0 = 静态采样器
//         t1 = 阴影图，t2 = 天空图（LightingUtil.hlsl），t3 = 法线图（只有定义了 NORMAL_MAP 的 PS 读，现在只有地板）
// ConstantBuffer<T> 需要 Shader Model 5.1（vs_5_1 / ps_5_1）

#include "LightingUtil.hlsl"   // Light、MaterialConstants、MaxLights、ComputeDirectionalLight

// 必须和 C++ 里 FrameResource.h 的同名结构体一一对应
struct ObjectConstants
{
    float4x4 World;
    float4x4 WorldInvTranspose;
    float4x4 PrevWorld; // 上一帧的 World（TAA 速度缓冲）
};

ConstantBuffer<ObjectConstants> gObj : register(b0);
ConstantBuffer<PassConstants> gPass : register(b1);
ConstantBuffer<MaterialConstants> gMat : register(b2);

Texture2D gDiffuseMap : register(t0);
Texture2D gNormalMap : register(t3);

// 必须和 C++ 里的 Vertex 结构体、输入布局一致
struct VertexIn
{
    float3 PosL : POSITION; // L = Local，局部空间
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
    float3 TangentU : TANGENT;
};

struct VertexOut
{
    float4 PosH : SV_POSITION; // H = Homogeneous，齐次裁剪空间
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD;
    float3 PosW : TEXCOORD1;
    float3 TangentW : TANGENT;
    float4 CurPosH : TEXCOORD2; //这一帧的裁剪坐标（不带抖动）
    float4 PrevPosH : TEXCOORD3;    //上一帧的
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    float4 posW = mul(float4(vin.PosL, 1.0f), gObj.World); // 局部 → 世界
    vout.PosH = mul(posW, gPass.ViewProj); // 世界 → 裁剪空间
    vout.NormalW = mul(vin.NormalL, (float3x3)gObj.WorldInvTranspose); // 转到世界空间
    vout.TexC = vin.TexC;
    vout.PosW = posW.xyz;
    vout.TangentW = mul(vin.TangentU,(float3x3)gObj.World);
    vout.CurPosH = mul(posW, gPass.ViewProjNoJitter);
    vout.PrevPosH = mul(mul(float4(vin.PosL, 1.0f), gObj.PrevWorld), gPass.PrevViewProjNoJitter);
    return vout;
}

// 这个像素的表面是什么：材质（基础色已乘贴图）+ 法线（已过法线贴图）。前向 PS 和 PS_GBuffer 共用
void GetSurface(VertexOut pin, out MaterialConstants mat, out float3 normal)
{
    #ifdef NO_DIFFUSE_MAP
        float4 TexColor = 1.0f; // PBR 测试球：颜色全来自材质
    #else
        float4 TexColor = gDiffuseMap.Sample(gsamLinearWrap, pin.TexC); // Sample 只能在像素着色器里用
    #endif
    mat = gMat;
    mat.BaseColor = TexColor * gMat.BaseColor;
    #ifdef ALPHA_TEST
        clip(mat.BaseColor.a -0.1f);
    #endif
        normal = normalize(pin.NormalW);   // 插值之后长度不一定是 1
    #ifdef NORMAL_MAP
        float4 normalMapSample = gNormalMap.Sample(gsamLinearWrap, pin.TexC);
        normal = NormalSampleToWorldSpace(normalMapSample.rgb, normal, pin.TangentW);
        mat.Roughness = 1.0f - (1.0f - mat.Roughness) * normalMapSample.a; // alpha 是光泽度：1 = 照常反光，0 = 瓷砖缝里不反光
    #endif
}

// 前向：表面 + 算光
float4 PS(VertexOut pin) : SV_Target
{
    MaterialConstants mat;
    float3 normal;
    GetSurface(pin, mat, normal);

    float3 toEye = gPass.EyePosW - pin.PosW;
    float distToEye = length(toEye);
    toEye /= distToEye;
    float3 ambient = AmbientLighting(mat, normal, toEye, gPass); // 环境光：IBL 或常数（面板"IBL"）
    float3 shadowFactor = float3(1.0f, 1.0f, 1.0f);
    shadowFactor[0] = CalcShadowFactor(gPass, pin.PosW, normal); // Lights[0] 是太阳；级联阴影按离相机多远选级联
    float3 direct = ComputeLighting(gPass.Lights, mat, pin.PosW, normal, toEye,shadowFactor);
    float4 res = float4((ambient + direct) * CascadeDebugTint(gPass, pin.PosW), mat.BaseColor.a);
#ifdef FOG
    res.rgb = ApplyFog(res.rgb, distToEye, FogColorAt(toEye), gPass.FogStart, gPass.FogRange);
#endif
    return res;
}

// 延迟：只把表面写进 G-Buffer，不算光（光照 Pass 再算）
GBufferOut PS_GBuffer(VertexOut pin)
{
    MaterialConstants mat;
    float3 normal;
    GetSurface(pin, mat, normal);
    return PackGBuffer(mat, normal, MotionVector(pin.CurPosH, pin.PrevPosH));
}

VertexOut VS_MirrorClear(VertexIn vin)
{
    VertexOut vout = VS(vin);            // 位置照常算
    vout.PosH.z = vout.PosH.w * 0.9999f; // 透视除法后 z/w ≈ 1，也就是最远处
    return vout;
}

float4 PS_MirrorClear(VertexOut pin) : SV_Target
{
    // 镜子后面的"背景"：视线打到镜面上弹出去，看到的是那个方向的天空
    float3 toEye = normalize(gPass.EyePosW - pin.PosW);
    float3 r = SkyDir(reflect(-toEye, normalize(pin.NormalW)));
    return gCubeMap.Sample(gsamLinearWrap, r);
}

// 阴影 Pass：只要深度。铁丝网的洞要 clip 掉，不然影子是一整块
void PS_Shadow(VertexOut pin)
{
    float4 TexColor = gDiffuseMap.Sample(gsamLinearWrap, pin.TexC); // Sample 只能在像素着色器里用
    clip(TexColor.a * gMat.BaseColor.a - 0.1f);
}
