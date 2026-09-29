// 受光的无贴图物体（陆地和水面）：颜色只来自材质（DiffuseAlbedo）和光照，不用顶点色
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
struct PassConstants
{
    float4x4 ViewProj;
    float3 EyePosW;
    float padding;
    float4 AmbientLight;
    Light Lights[MaxLights];
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
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    float4 posW = mul(float4(vin.PosL, 1.0f), gObj.World); // 局部 → 世界
    vout.PosH = mul(posW, gPass.ViewProj); // 世界 → 裁剪空间
    vout.NormalW = mul(vin.NormalL, (float3x3)gObj.WorldInvTranspose); // 转到世界空间
    vout.PosW = posW.xyz;
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    // 没有贴图，材质原样拿来用
    float3 ambient = gPass.AmbientLight.rgb * gMat.DiffuseAlbedo.rgb;
    float3 toEye = normalize(gPass.EyePosW - pin.PosW);
    float3 normal = normalize(pin.NormalW);   // 插值之后长度不一定是 1
    float3 direct = ComputeLighting(gPass.Lights, gMat, pin.PosW, normal, toEye);
    return float4(ambient + direct, gMat.DiffuseAlbedo.a);
}
