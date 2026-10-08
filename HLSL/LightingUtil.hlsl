// 光照工具：Light、MaterialConstants 和各种灯的计算，Triangle.hlsl / Color.hlsl 共用
// 用法：在着色器里 #include "LightingUtil.hlsl"，再声明 ConstantBuffer<MaterialConstants> gMat : register(b2)

#define MaxLights 100   // 必须和 C++ FrameResource.h 里的 MaxLights 一致
static const float PI = 3.14159265f;
static const float MinRoughness = 0.045f; // 感知粗糙度的下限

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
    float4 BaseColor;
    float Metallic;
    float Roughness;
    float Reflectance;
    float Pad;
};

struct PassConstants
{
    float4x4 ViewProj;
    float4x4 ShadowTransform;
    float4x4 InvViewProj;
    float3 EyePosW;
    float padding;
    float4 AmbientLight;
    float4 FogColor;
    float FogStart;
    float FogRange;
    Light Lights[MaxLights];
};

Texture2D gShadowMap : register(t1);
TextureCube gCubeMap : register(t2); // 天空图：用方向去查
SamplerState gsamLinearWrap : register(s0);
SamplerComparisonState gsamShadow : register(s1);

//金属没有漫反射
float3 DiffuseColor(MaterialConstants mat){
    return (1 - mat.Metallic)*mat.BaseColor.rgb;
}

// 正对着看的反射率：电介质是 0.16·r²（白色），金属是基础色；按 Metallic 在两者之间过渡
float3 F0(MaterialConstants mat)
{
    return lerp(0.16*pow(mat.Reflectance,2),mat.BaseColor.rgb,mat.Metallic);
}

// D：法线正好朝着 h 的小镜面占多少（GGX）。参数 a 就是 α，已经平方过了
float D_GGX(float NoH, float a)
{
    float a2 = a * a;
    float f = NoH * NoH * (a2 - 1) + 1;   // 空3：公式分母括号里那一项，(n·h)²(α² − 1) + 1
    return a2 / (PI * f * f);
}

// V = G / (4·NoV·NoL)：照抄 Filament 的代码清单
float V_SmithGGXCorrelated(float NoV, float NoL, float a)
{
    float a2 = a * a;
    float GGXL = NoV * sqrt((-NoL * a2 + NoL) * NoL + a2);
    float GGXV = NoL * sqrt((-NoV * a2 + NoV) * NoV + a2);
    return 0.5f / (GGXV + GGXL);
}

// F：Schlick。u = l·h
float3 F_Schlick(float u, float3 f0)
{
    return f0 + (1-f0)*pow((1-u),5);
}

// 一盏灯照到这个点：(漫反射 + 镜面) × 光强 × NoL，三种灯共用
//   灯的强度沿用以前的意思：正对着照一块纯白漫反射面时显示的亮度
float3 ShadePBR(float3 lightStrength, float3 lightVec, float3 normal, float3 toEye, MaterialConstants mat)
{
    float3 h = normalize(toEye + lightVec);
    float NoV = abs(dot(normal,toEye))+1e-5;   // 空5：照 Filament 的写法。为什么这么写，看图里第 6 种
    float NoL = saturate(dot(normal, lightVec));
    float NoH = saturate(dot(normal, h));
    float LoH = saturate(dot(lightVec, h));
    float r = max(MinRoughness,mat.Roughness);     // 空6：粗糙度夹到下限以上
    float a = r*r;     // 空7：感知粗糙度 → α
    float3 Fr = D_GGX(NoH, a) * V_SmithGGXCorrelated(NoV, NoL, a) * F_Schlick(LoH, F0(mat));
    float3 Fd = DiffuseColor(mat) / PI;   // 空8：Lambert
    return (Fr + Fd)*PI * lightStrength * NoL;        // 空9：别忘了乘 π
}

// 平行光：光向量处处一样，没有衰减
float3 ComputeDirectionalLight(Light L, MaterialConstants mat, float3 normal, float3 toEye)
{
    float3 lightVec = -L.Direction;
    return ShadePBR(L.Strength, lightVec, normal, toEye, mat);
}

// 平方反比 × 窗口：光强在 radius 处平滑地降到 0；灯贴着表面时也不会除以 0
float DistanceAttenuation(float d, float radius)
{
    float f = pow(d/radius,2);                          // 空11：公式里要的是 (d/r)⁴，这里先算 (d/r)²
    float window = saturate(1.0f - f * f);
    return window * window / max(d * d, 1e-4f); // 空12
}

// 点光源：灯挂在 L.Position，光向量每个点都不同，所以需要这个点的世界坐标 pos
float3 ComputePointLight(Light L, MaterialConstants mat, float3 pos, float3 normal, float3 toEye)
{
    float3 result = float3(0.0f, 0.0f, 0.0f); // 太远的点照不到，结果就是 0
    float3 lightVec = L.Position - pos; // 从表面指向灯
    float d = length(lightVec);
    if (d <= L.FalloffEnd)
    {
        lightVec /= d; // 归一化（前面已经算过长度，直接除）
        float3 lightStrength = L.Strength * DistanceAttenuation(d, L.FalloffEnd);
        result = ShadePBR(lightStrength, lightVec, normal, toEye, mat);
    }
    return result;
}

// 聚光灯：点光源 + 一个锥形的罩子。L.Direction 是锥的中轴（光往哪照），L.SpotPower 越大锥越窄
float3 ComputeSpotLight(Light L, MaterialConstants mat, float3 pos, float3 normal, float3 toEye)
{
    float3 result = float3(0.0f, 0.0f, 0.0f);
    float3 lightVec = L.Position - pos; // 前半段和点光源一模一样
    float d = length(lightVec);
    if (d <= L.FalloffEnd)
    {
        lightVec /= d;
        float3 lightStrength = L.Strength * DistanceAttenuation(d, L.FalloffEnd);

        // -lightVec 是"灯 → 这个点"的方向，和锥的中轴越接近，点积越接近 1。
        // 再 pow 一下：SpotPower 越大，偏离中轴时掉得越快，光圈越小
        float spotFactor = pow(max(dot(-lightVec, L.Direction), 0.0f), L.SpotPower);
        lightStrength *= spotFactor;

        result = ShadePBR(lightStrength, lightVec, normal, toEye, mat);
    }
    return result;
}

// 把所有灯的贡献加起来。灯在 Lights[] 里的排布：先平行光，再点光源，最后聚光灯
// NUM_DIR_LIGHTS / NUM_POINT_LIGHTS / NUM_SPOT_LIGHTS 由 C++ 编译着色器时用宏传进来
float3 ComputeLighting(Light lights[MaxLights], MaterialConstants mat, float3 pos, float3 normal, float3 toEye,
                       float3 shadowFactor)
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

struct GBufferOut{
    float4 BaseMetal : SV_Target0;
    float4 NormalRough : SV_Target1;
};

GBufferOut PackGBuffer(MaterialConstants mat,float3 normal){
    GBufferOut o;
    o.BaseMetal = float4(mat.BaseColor.rgb,mat.Metallic);
    o.NormalRough = float4(normal,mat.Roughness);
    return o;
}

// 线性雾：距离 <= fogStart 不受影响，>= fogStart + fogRange 完全变成雾色，中间线性过渡。
// 只处理 RGB，alpha 由调用方保留（半透明的水不能被雾改掉透明度）
float3 ApplyFog(float3 color, float distToEye, float4 fogColor, float fogStart, float fogRange)
{
    float s = saturate((distToEye - fogStart) / fogRange);
    return lerp(color, fogColor.rgb, s);
}

// 天空图相对世界绕 y 轴转了 41.4°（让图里的太阳对上 mSunDir）
// 世界里的方向 → 天空图里的方向：反着转回去。所有查天空图的地方都先过这一道
static const float SkyYaw = radians(41.4f);

float3 SkyDir(float3 dirW)
{
    float s = -sin(SkyYaw);
    float c = cos(SkyYaw);
    return float3(c * dirW.x + dirW.z * s, dirW.y, -s * dirW.x + c * dirW.z);
}

// 雾色：沿视线方向查天空图，用很糊的第 5 级 mip（32×32），远处的东西融进"大概的天空颜色"
float4 FogColorAt(float3 toEye)
{
    float3 dir = SkyDir(-toEye);
    return float4(gCubeMap.SampleLevel(gsamLinearWrap, dir, 5).rgb, 1.0f);
}

// 环境反射：把视线在表面上弹一下，用弹出去的方向查天空图
float3 EnvReflection(MaterialConstants mat, float3 normal, float3 toEye)
{
    float3 r = reflect(-toEye, normal);
    float3 reflectionColor = gCubeMap.Sample(gsamLinearWrap, SkyDir(r)).rgb;
    return (1.0f - mat.Roughness) * F_Schlick(saturate(dot(normal,toEye)), F0(mat)) * reflectionColor;
}

// 法线图的一个样本（切线空间）→ 世界空间
float3 NormalSampleToWorldSpace(float3 normalMapSample, float3 unitNormalW, float3 tangentW)
{
    float3 normalT = normalMapSample *2 -1; // [0,1] → [-1,1]
    float3 N = unitNormalW;
    float3 T = normalize(tangentW - dot(tangentW , N)*N); // 扶正：减掉 T 在 N 上的投影
    float3 B = cross(N, T);
    float3x3 TBN = float3x3(T, B, N); // 三行：T、B、N
    return mul(normalT,TBN); // x·T + y·B + z·N
}

// 返回 0～1：1 = 全亮，0 = 全在影子里。shadowPosH = 这个点在阴影图里的 (u, v, 深度, w)
float CalcShadowFactor(float4 shadowPosH)
{
    shadowPosH.xyz /= shadowPosH.w; //正交投影 w = 1，但是聚光灯就不一定w=1了，所以这句还是要写
    float depth = shadowPosH.z;
    uint width, height, numMips;
    gShadowMap.GetDimensions(0, width, height, numMips);
    float dx = 1.0f / (float)width; //一个纹素在uv里有多宽

    const float2 offsets[9] = {
        float2(-dx, -dx), float2(0, -dx), float2(dx, -dx),
        float2(-dx, 0), float2(0, 0), float2(dx, 0),
        float2(-dx, dx), float2(0, dx), float2(dx, dx)
    };
    float percentLit = 0.0f;
    [unroll] for (int i = 0; i < 9; ++i)
        percentLit += gShadowMap.SampleCmpLevelZero(
            gsamShadow, shadowPosH.xy + offsets[i], depth);
    return percentLit / 9.0f;
}
