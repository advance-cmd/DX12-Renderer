// 光照工具：Light、MaterialConstants 和各种灯的计算，Triangle.hlsl / Color.hlsl 共用
// 用法：在着色器里 #include "LightingUtil.hlsl"，再声明 ConstantBuffer<MaterialConstants> gMat : register(b2)

#define MaxLights 100   // 必须和 C++ FrameResource.h 里的 MaxLights 一致

// 必须和 C++ FrameResource.h 的 Light 一一对应（每两个字段正好一行 16 字节，共 48 字节）
struct Light
{
    float3 Strength;
    float FalloffStart;

    float3 Direction;
    float FalloffEnd;

    float3 Position;
    float SpotPower;
};

// 必须和 C++ FrameResource.h 的 MaterialConstants 一一对应
struct MaterialConstants
{
    float4 DiffuseAlbedo;
    float3 FresnelR0;
    float Roughness;
};

// 平行光：现在只有漫反射。toEye（表面指向眼睛的方向）留给高光用
float3 ComputeDirectionalLight(Light L, MaterialConstants mat, float3 normal, float3 toEye)
{
    float3 LightDir = -L.Direction;
    float3 VertexNormal = normalize(normal);
    return mat.DiffuseAlbedo.rgb * L.Strength * max(dot(LightDir, VertexNormal), 0.0f);
}
