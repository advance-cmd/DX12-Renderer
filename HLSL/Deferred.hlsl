// 延迟渲染：读 G-Buffer 的全屏 Pass（光照 Pass + 调试视图）
// 根签名用主根签名：b1 = Pass 常量，t6～t8 = 两张 G-Buffer + 深度缓冲（根参数 8，堆第 25～27 格）
// 光照 Pass 还要用 LightingUtil.hlsl 里的 t1 阴影图、t2 天空图（雾色），Draw() 进 DrawDeferred 之前已经绑好
#include "LightingUtil.hlsl"

ConstantBuffer<PassConstants> gPass : register(b1);

Texture2D gGBuffer0 : register(t6);        // rgb 基础色，a 金属度（sRGB 格式，读出来已经换回线性值）
Texture2D gGBuffer1 : register(t7);        // xyz 世界法线，w 粗糙度
Texture2D<float> gDepthMap : register(t8); // 深度缓冲（R24_UNORM_X8_TYPELESS：只读前 24 位深度）

struct VertexOut
{
    float4 PosH : SV_Position;
    float2 UV : TEXCOORD;
};

// 和 Tonemap.hlsl 的 VS 一样：DrawInstanced(3) 时 id = 0、1、2，一个三角形盖住全屏
VertexOut VS(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2); // id 0、1、2 → uv (0,0)、(2,0)、(0,2)
    VertexOut vout;
    vout.PosH = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    vout.UV = uv;
    return vout;
}

// G-Buffer 里这个像素的表面。Reflectance 没存：走延迟的材质都是 0.5
void ReadGBuffer(int2 pixel, out MaterialConstants mat, out float3 normal)
{
    float4 g0 = gGBuffer0.Load(int3(pixel, 0));
    float4 g1 = gGBuffer1.Load(int3(pixel, 0));
    mat.BaseColor = float4(g0.rgb, 1.0f);
    mat.Metallic = g0.a;
    mat.Roughness = g1.w;
    mat.Reflectance = 0.5f;
    mat.Pad = 0.0f;
    normal = g1.xyz;
}

float3 WorldPosFromDepth(float2 uv, float depth)
{
    // UV 左上(0,0) -> NDC(-1,+1)
    float2 ndc = uv * float2(2, -2) + float2(-1, 1);

    // 反变换后仍是齐次坐标
    float4 worldH = mul(float4(ndc, depth, 1.0f), gPass.InvViewProj);
    return worldH.xyz / worldH.w;
}

float4 PS(VertexOut pin) : SV_Target
{
    // SV_Position.xy 已经是像素中心，例如 (100.5, 50.5)
    // 取整后用于 Texture2D.Load
    int2 pixel = int2(pin.PosH.xy);
    float depth = gDepthMap.Load(int3(pixel, 0));

    // 深度清除值是 1；没有不透明表面的位置留给后面的天空
    if (depth >= 1.0f)
        discard;

    MaterialConstants mat;
    float3 normal;
    ReadGBuffer(pixel, mat, normal);

    float3 posW = WorldPosFromDepth(pin.UV, depth);

    // 以下沿用前向 PS 的光照计算
    float3 ambient = gPass.AmbientLight.rgb * (DiffuseColor(mat) + F0(mat));

    float3 toEye = gPass.EyePosW - posW;
    float distToEye = length(toEye);
    toEye /= distToEye;

    float3 shadowFactor = float3(1.0f, 1.0f, 1.0f);
    shadowFactor[0] = CalcShadowFactor(mul(float4(posW, 1.0f), gPass.ShadowTransform));
    float3 direct = ComputeLighting(gPass.Lights, mat, posW, normal, toEye, shadowFactor);
    float3 color = ambient + direct;
#ifdef FOG
    color = ApplyFog(color, distToEye, FogColorAt(toEye), gPass.FogStart, gPass.FogRange);
#endif

    return float4(color, 1.0f);
}

// ======================== 光体积 ========================
static const uint kFirstExtraLight = NUM_DIR_LIGHTS + NUM_POINT_LIGHTS + NUM_SPOT_LIGHTS;

struct VolumeIn
{
    float3 PosL : POSITION; //球网格只用位置
};

struct VolumeOut
{
    float4 PosH : SV_Position;
    nointerpolation uint Light : LIGHT; //第几盏灯
};

VolumeOut VS_LightVolume(VolumeIn vin,uint id : SV_InstanceID){
    Light L = gPass.Lights[kFirstExtraLight + id];
    // 球网格就是天空那个：CreateSphere(.5f, 20, 20)。要把它变成半径 = 影响半径的球，再放大 5%
    float3 posW = vin.PosL * 2.0f * L.FalloffEnd * 1.05f + L.Position;
    VolumeOut vout;
    vout.PosH = mul(float4(posW,1.0f),gPass.ViewProj);
    vout.Light = kFirstExtraLight + id;
    return vout;
}

float4 PS_LightVolume(VolumeOut pin) : SV_Target{
    int2 pixel = int2(pin.PosH.xy);
    float depth = gDepthMap.Load(int3(pixel,0));
    if(depth >=1.0f)
        discard;
    
    uint width, height;
    gDepthMap.GetDimensions(width, height);
    float2 uv = pin.PosH.xy / float2(width, height);
    
    MaterialConstants mat;
    float3 normal;
    ReadGBuffer(pixel,mat,normal);
    float3 posW = WorldPosFromDepth(uv,depth);
    float3 toEye = gPass.EyePosW - posW;
    float distToEye = length(toEye);
    toEye/=distToEye;
    float3 c = ComputePointLight(gPass.Lights[pin.Light], mat, posW, normal, toEye);
#ifdef FOG
    c *= 1.0f - saturate((distToEye - gPass.FogStart) / gPass.FogRange);
#endif
    return float4(c, 0.0f); // alpha 用不上
}
// ======================== 调试视图（面板上的"G-Buffer 视图"） ========================
// 色调映射这时临时换成"曝光 0 EV + 截断"，这里输出的线性值经 sRGB 编码直接上屏：
// 基础色本来就是线性颜色，原样输出；法线、粗糙度这些"数"先做 2.2 次方，上屏后的灰度才等于原值
#ifdef GBUFFER_VIEW
float4 PS_GBufferView(VertexOut pin) : SV_Target
{
    int2 pixel = int2(pin.PosH.xy); // SV_Position.xy 是像素中心（x + 0.5），取整就是第几个像素
    float depth = gDepthMap.Load(int3(pixel, 0));
    MaterialConstants mat;
    float3 normal;
    ReadGBuffer(pixel, mat, normal);
#if GBUFFER_VIEW == 1
    return float4(mat.BaseColor.rgb, 1.0f);
#else
    float3 c;
#if GBUFFER_VIEW == 2
    c = normal * 0.5f + 0.5f; // [-1, 1] → [0, 1]
#elif GBUFFER_VIEW == 3
    c = mat.Roughness;
#elif GBUFFER_VIEW == 4
    c = mat.Metallic;
#elif GBUFFER_VIEW == 5
    // 深度缓冲里的值和距离不成正比（近处变得快，远处全挤在 1 附近），换回观察空间的 z 再显示。近平面 1、远平面 1000
    float viewZ = 1000.0f / (1000.0f - 999.0f * depth);
    c = depth >= 1.0f ? 1.0f : saturate(viewZ / 150.0f);
#else
    // 世界坐标：每 2 个单位一条白色格线（xz 平面），底色按每 8 个单位循环。
    // 反推对了，挪相机时格子钉在物体表面上；反推错了，格子会跟着视角滑动、变形
    if (depth >= 1.0f)
        return float4(0.0f, 0.0f, 0.0f, 1.0f); // 天空：没有表面
    float3 p = WorldPosFromDepth(pin.UV, depth); // 第②步你写的那个函数
    float3 f = abs(frac(p / 2.0f + 0.5f) - 0.5f) * 2.0f; // 到最近格线的距离，格线上是 0
    float onGrid = min(f.x, f.z) < 0.06f ? 1.0f : 0.0f;
    c = lerp(frac(p / 8.0f), 1.0f, onGrid);
#endif
    return float4(pow(c, 2.2f), 1.0f);
#endif
}
#endif


