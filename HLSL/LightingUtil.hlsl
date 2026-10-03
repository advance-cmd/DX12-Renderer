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

struct PassConstants
{
    float4x4 ViewProj;
    float4x4 ShadowTransform;
    float3 EyePosW;
    float padding;
    float4 AmbientLight;
    float4 FogColor;
    float FogStart;
    float FogRange;
    Light Lights[MaxLights];
};

Texture2D gShadowMap : register(t1);
SamplerComparisonState gsamShadow : register(s1);

// Blinn-Phong：漫反射 + 高光，三种灯共用。各种灯只负责算出
//   lightStrength：这盏灯到达这个点时的强度（已经乘过衰减、聚光的锥形因子，还没乘夹角余弦）
//   lightVec：从表面指向灯的单位向量
// 约定：normal、lightVec、toEye 都是单位向量
float3 BlinnPhong(float3 lightStrength, float3 lightVec, float3 normal, float3 toEye, MaterialConstants mat)
{
    // Lambert 余弦：光斜着照，同样的光摊到更大的面积上，每一小块接到的就少；背面接不到光
    float ndotl = max(dot(lightVec, normal), 0.0f);

    float m = (1 - mat.Roughness) * 256;                // 光泽度：越光滑，高光越小越亮
    float3 h = normalize(toEye + lightVec);             // 半角向量
    float roughnessFactor = (m + 8) / 8 * pow(max(dot(h, normal), 0), m);
    // Schlick 近似的 Fresnel：夹角用 h 和 lightVec（Blinn-Phong 里微表面的法线就是 h）
    float3 fresnel = mat.FresnelR0 + (1 - mat.FresnelR0) * pow(1 - max(dot(h, lightVec), 0), 5);
    float3 specAlbedo = fresnel * roughnessFactor;
    specAlbedo = specAlbedo / (specAlbedo + 1);         // LDR：把可能超过 1 的高光压回 [0, 1)
    return (mat.DiffuseAlbedo.rgb + specAlbedo) * lightStrength * ndotl;
}

// 平行光：光向量处处一样，没有衰减
float3 ComputeDirectionalLight(Light L, MaterialConstants mat, float3 normal, float3 toEye)
{
    float3 lightVec = -L.Direction;
    return BlinnPhong(L.Strength, lightVec, normal, toEye, mat);
}

// 衰减：距离 <= falloffStart 时是 1，>= falloffEnd 时是 0，中间线性过渡
float CalcAttenuation(float d, float falloffStart, float falloffEnd)
{
    return saturate((falloffEnd - d) / (falloffEnd - falloffStart));
}

// 点光源：灯挂在 L.Position，光向量每个点都不同，所以需要这个点的世界坐标 pos
float3 ComputePointLight(Light L, MaterialConstants mat, float3 pos, float3 normal, float3 toEye)
{
    float3 result = float3(0.0f, 0.0f, 0.0f);           // 太远的点照不到，结果就是 0
    float3 lightVec = L.Position - pos;                 // 从表面指向灯
    float d = length(lightVec);
    if (d <= L.FalloffEnd)
    {
        lightVec /= d;                                  // 归一化（前面已经算过长度，直接除）
        float3 lightStrength = L.Strength * CalcAttenuation(d, L.FalloffStart, L.FalloffEnd);
        result = BlinnPhong(lightStrength, lightVec, normal, toEye, mat);
    }
    return result;
}

// 聚光灯：点光源 + 一个锥形的罩子。L.Direction 是锥的中轴（光往哪照），L.SpotPower 越大锥越窄
float3 ComputeSpotLight(Light L, MaterialConstants mat, float3 pos, float3 normal, float3 toEye)
{
    float3 result = float3(0.0f, 0.0f, 0.0f);
    float3 lightVec = L.Position - pos;                 // 前半段和点光源一模一样
    float d = length(lightVec);
    if (d <= L.FalloffEnd)
    {
        lightVec /= d;
        float3 lightStrength = L.Strength * CalcAttenuation(d, L.FalloffStart, L.FalloffEnd);

        // -lightVec 是"灯 → 这个点"的方向，和锥的中轴越接近，点积越接近 1。
        // 再 pow 一下：SpotPower 越大，偏离中轴时掉得越快，光圈越小
        float spotFactor = pow(max(dot(-lightVec, L.Direction), 0.0f), L.SpotPower);
        lightStrength *= spotFactor;

        result = BlinnPhong(lightStrength, lightVec, normal, toEye, mat);
    }
    return result;
}

// 把所有灯的贡献加起来。灯在 Lights[] 里的排布：先平行光，再点光源，最后聚光灯
// NUM_DIR_LIGHTS / NUM_POINT_LIGHTS / NUM_SPOT_LIGHTS 由 C++ 编译着色器时用宏传进来
float3 ComputeLighting(Light lights[MaxLights], MaterialConstants mat, float3 pos, float3 normal, float3 toEye,float3 shadowFactor)
{
    float3 result = 0.0f;
    int i = 0;

#if (NUM_DIR_LIGHTS > 0)
    [unroll]
    for (i = 0; i < NUM_DIR_LIGHTS; ++i)
        result += shadowFactor[i] * ComputeDirectionalLight(lights[i], mat, normal, toEye);
#endif

#if (NUM_POINT_LIGHTS > 0)
    [unroll]
    for (i = NUM_DIR_LIGHTS; i < NUM_DIR_LIGHTS + NUM_POINT_LIGHTS; ++i)
        result += ComputePointLight(lights[i], mat, pos, normal, toEye);
#endif

#if (NUM_SPOT_LIGHTS > 0)
    [unroll]
    for (i = NUM_DIR_LIGHTS + NUM_POINT_LIGHTS; i < NUM_DIR_LIGHTS + NUM_POINT_LIGHTS + NUM_SPOT_LIGHTS; ++i)
        result += ComputeSpotLight(lights[i], mat, pos, normal, toEye);
#endif

    return result;
}

// 线性雾：距离 <= fogStart 不受影响，>= fogStart + fogRange 完全变成雾色，中间线性过渡。
// 只处理 RGB，alpha 由调用方保留（半透明的水不能被雾改掉透明度）
float3 ApplyFog(float3 color, float distToEye, float4 fogColor, float fogStart, float fogRange)
{
    float s = saturate((distToEye - fogStart) / fogRange);
    return lerp(color, fogColor.rgb, s);
}

// 返回 0～1：1 = 全亮，0 = 全在影子里。shadowPosH = 这个点在阴影图里的 (u, v, 深度, w)
float CalcShadowFactor(float4 shadowPosH)
{
    shadowPosH.xyz /=shadowPosH.w;  //正交投影 w = 1，但是聚光灯就不一定w=1了，所以这句还是要写
    float depth = shadowPosH.z;
    uint width,height,numMips;
    gShadowMap.GetDimensions(0,width,height,numMips);
    float dx = 1.0f / (float)width;    //一个纹素在uv里有多宽
    
    const float2 offsets[9] = {
        float2(-dx,-dx), float2(0,-dx), float2(dx,-dx),
        float2(-dx, 0),  float2(0, 0),  float2(dx, 0),
        float2(-dx, dx), float2(0, dx), float2(dx, dx) };
    float percentLit = 0.0f;
    [unroll] for (int i = 0; i < 9; ++i)
        percentLit += gShadowMap.SampleCmpLevelZero(
            gsamShadow, shadowPosH.xy + offsets[i], depth);
    return percentLit / 9.0f;
}