// 受光的无贴图物体（陆地和水面）：颜色只来自材质（BaseColor）和光照，不用顶点色
// 根签名和 Triangle.hlsl 共用：b0 = 物体常量，b1 = Pass 常量，b2 = 材质
// 这个 shader 不用纹理，所以根参数 1 的 SRV 表设不设都行
// 输入布局里还有 COLOR，但这里不读它：输入布局可以比 VertexIn 多，按语义名对应
// ConstantBuffer<T> 需要 Shader Model 5.1

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

// 必须和 C++ 里的 ColorVertex 结构体、输入布局一致
struct VertexIn
{
    float3 PosL : POSITION; // L = Local，局部空间
    float3 NormalL : NORMAL;
};

struct VertexOut
{
    float4 PosH : SV_POSITION; // H = Homogeneous，齐次裁剪空间
    float3 NormalW : NORMAL;
    float3 PosW : TEXCOORD;
    float4 shadowPosH : TEXCOORD1;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    float4 posW = mul(float4(vin.PosL, 1.0f), gObj.World); // 局部 → 世界
    vout.PosH = mul(posW, gPass.ViewProj); // 世界 → 裁剪空间
    vout.NormalW = mul(vin.NormalL, (float3x3)gObj.WorldInvTranspose); // 转到世界空间
    vout.PosW = posW.xyz;
    vout.shadowPosH = mul(posW,gPass.ShadowTransform);
    return vout;
}

#ifdef LAND_TEX
Texture2D gGrassMap : register(t4);
Texture2D gStoneMap : register(t5);
#endif

float4 PS(VertexOut pin) : SV_Target
{
    MaterialConstants mat = gMat; // 常量缓冲是只读的，要改就复制一份
    
#ifdef LAND_TEX
    // ① 陆地的顶点没有 uv，就拿世界坐标的水平方向 (x, z) 当 uv，相当于从正上方把纹理"投"到地面上。
    //    除以 6：每 6 个单位铺一块。比如 (x, z) = (12, -30) → uv = (2, -5)；采样器是 Wrap 模式，只看小数部分，所以每 6 个单位重复一次
    float2 uv = pin.PosW.xz / 6.0f;

    // ② 两张纹理都采样一次。石头的 uv 再乘 0.5，块就大一倍
    float3 grass = gGrassMap.Sample(gsamLinearWrap, uv).rgb;
    float3 stone = gStoneMap.Sample(gsamLinearWrap, uv * 0.5f).rgb;

    // ③ 坡有多陡：法线的 y 分量就是坡面和水平面夹角的余弦
    //    平地 y = 1，45° 的坡 y ≈ 0.71，竖直的崖 y = 0
    //    想要：y ≥ 0.85 全是草（0），y ≤ 0.65 全是石头（1），中间按比例过渡
    float steep = saturate((0.85f - normalize(pin.NormalW).y) / 0.2f);

    // ④ lerp(a, b, t) = a × (1 - t) + b × t：steep = 0 是草，1 是石头
    //    材质颜色已经改成了 (1, 1, 1)，乘完就是纹理本身的颜色
    mat.BaseColor.rgb *= lerp(grass, stone, steep);
#endif
    
    // 没有贴图，材质原样拿来用
    float4 res=float4(0,0,0,0);
    float3 ambient = gPass.AmbientLight.rgb * (DiffuseColor(mat) + F0(mat));
    float3 toEye = gPass.EyePosW - pin.PosW;
    float distToEye = length(toEye);
    toEye /= distToEye;
    float3 normal = normalize(pin.NormalW);   // 插值之后长度不一定是 1
    float3 shadowFactor = float3(1.0f, 1.0f, 1.0f);
    shadowFactor[0] = CalcShadowFactor(pin.shadowPosH); // Lights[0] 是太阳，阴影图是站在太阳那里画的
    float3 direct = ComputeLighting(gPass.Lights, mat, pin.PosW, normal, toEye,shadowFactor);
    
    res = float4(ambient + direct, mat.BaseColor.a);
#ifdef ENV_REFLECT
    res.rgb += EnvReflection(mat, normal, toEye);   // 水面多出一份"从天空反射来的光"
#endif
#ifdef FOG
    res.rgb = ApplyFog(res.rgb, distToEye, FogColorAt(toEye), gPass.FogStart, gPass.FogRange);
#endif
    return res;
}
