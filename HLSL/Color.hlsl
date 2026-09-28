// 顶点色物体（陆地和水面）：VS 做 局部 → 世界 → 裁剪空间，PS 直接返回顶点自带的颜色
// 根签名和 Triangle.hlsl 共用：b0 = 物体常量，b1 = Pass 常量
// 这个 shader 不用纹理，所以根参数 1 的 SRV 表设不设都行
// ConstantBuffer<T> 需要 Shader Model 5.1

// 必须和 C++ 里 FrameResource.h 的同名结构体一一对应
struct ObjectConstants { float4x4 World; };      // 每个物体一份
struct PassConstants   { float4x4 ViewProj; };   // 每个 Pass 一份，所有物体共用

ConstantBuffer<ObjectConstants> gObj  : register(b0);
ConstantBuffer<PassConstants>   gPass : register(b1);

// 必须和 C++ 里的 ColorVertex 结构体、输入布局一致
struct VertexIn
{
    float3 PosL  : POSITION;   // L = Local，局部空间
    float4 Color : COLOR;
};

struct VertexOut
{
    float4 PosH  : SV_POSITION;  // H = Homogeneous，齐次裁剪空间
    float4 Color : COLOR;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    float4 posW = mul(float4(vin.PosL, 1.0f), gObj.World);   // 局部 → 世界
    vout.PosH   = mul(posW, gPass.ViewProj);                 // 世界 → 裁剪空间
    vout.Color  = vin.Color;                                 // 颜色原样传下去，PS 直接输出
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    return pin.Color;
}
