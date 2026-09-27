// 带纹理的物体：VS 做局部 → 世界 → 裁剪空间，PS 采样漫反射纹理
// 根签名：b0 = 物体常量（根 CBV），t0 = 纹理（描述符表），b1 = Pass 常量（根 CBV），s0 = 静态采样器
// ConstantBuffer<T> 需要 Shader Model 5.1（vs_5_1 / ps_5_1）

// 必须和 C++ 里 FrameResource.h 的同名结构体一一对应
struct ObjectConstants { float4x4 World; };      // 每个物体一份
struct PassConstants   { float4x4 ViewProj; };   // 每个 Pass 一份，所有物体共用

ConstantBuffer<ObjectConstants> gObj  : register(b0);
ConstantBuffer<PassConstants>   gPass : register(b1);

Texture2D    gDiffuseMap    : register(t0);
SamplerState gsamLinearWrap : register(s0);

// 必须和 C++ 里的 Vertex 结构体、输入布局一致
struct VertexIn
{
    float3 PosL : POSITION;     // L = Local，局部空间
    float2 TexC : TEXCOORD;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;  // H = Homogeneous，齐次裁剪空间
    float2 TexC : TEXCOORD;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    float4 posW = mul(float4(vin.PosL, 1.0f), gObj.World);   // 局部 → 世界
    vout.PosH   = mul(posW, gPass.ViewProj);                 // 世界 → 裁剪空间
    vout.TexC   = vin.TexC;
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    return gDiffuseMap.Sample(gsamLinearWrap, pin.TexC);   // Sample 只能在像素着色器里用
}
