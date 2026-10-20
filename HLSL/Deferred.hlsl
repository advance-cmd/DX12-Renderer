// 延迟渲染：读 G-Buffer 的全屏 Pass（光照 Pass + 调试视图）
// 根签名用主根签名：b1 = Pass 常量，t6～t8 = 两张 G-Buffer + 深度缓冲（根参数 8，堆第 25～27 格），t9 = SSAO 遮蔽图（根参数 9）
// s2（双线性 + 夹边）和 IBL 的 t10～t12 声明在 LightingUtil.hlsl 里
// 光照 Pass 还要用 LightingUtil.hlsl 里的 t1 阴影图、t2 天空图（雾色），Draw() 进 DrawDeferred 之前已经绑好
#include "LightingUtil.hlsl"

ConstantBuffer<PassConstants> gPass : register(b1);

Texture2D gGBuffer0 : register(t6);        // rgb 基础色，a 金属度（sRGB 格式，读出来已经换回线性值）
Texture2D gGBuffer1 : register(t7);        // xyz 世界法线，w 粗糙度
Texture2D<float> gDepthMap : register(t8); // 深度缓冲（R24_UNORM_X8_TYPELESS：只读前 24 位深度）
Texture2D<float> gAoMap : register(t9);    // SSAO 的遮蔽图（半分辨率，R8）：1 = 完全没被挡，0 = 全挡住。根参数 9

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
    float3 toEye = gPass.EyePosW - posW;
    float distToEye = length(toEye);
    toEye /= distToEye;

    // 以下沿用前向 PS 的光照计算
    float3 ambient = AmbientLighting(mat, normal, toEye, gPass); // 环境光：IBL 或常数（面板"IBL"）
    // SSAO：遮蔽图是半分辨率，用双线性 + 夹边的采样器放大回全屏。只压环境光，直接光不受影响
    ambient *= gAoMap.SampleLevel(gsamLinearClamp, pin.UV, 0);

    float3 shadowFactor = float3(1.0f, 1.0f, 1.0f);
    shadowFactor[0] = CalcShadowFactor(gPass, posW, normal); // 级联阴影：按离相机多远选级联
    float3 direct = ComputeLighting(gPass.Lights, mat, posW, normal, toEye, shadowFactor);
    float3 color = (ambient + direct) * CascadeDebugTint(gPass, posW); // 面板"级联上色"关着时乘 1
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
// ======================== SSAO ========================
// 环境光是"从四面八方来的光"。一个点周围挤着别的东西（墙角、箱子底下、缝里），能照到它的环境光就少。
// 遮蔽 Pass 对每个像素估一个 0～1 的数（1 = 四周空旷），模糊两趟去掉噪点，光照 Pass 再拿它去乘环境光。

// ---- 数据和小工具（AI 写的） ----
static const int kAoSamples = 14;         // 每个像素在周围试 14 个点
static const float kAoRadius = 0.5f;      // 试的点离这个像素多远（世界单位）
static const float kAoFadeStart = 0.2f;   // 挡住我的东西离我这么近以内：算完全挡住
static const float kAoFadeEnd = 1.0f;     // 远到这么多：不算挡（中间线性过渡）
static const float kAoEpsilon = 0.05f;    // 比这还近：当成我自己所在的那个面，不算
static const float kAoPower = 4.0f;       // 最后做几次方：越大，暗的地方越暗

// 14 个方向：立方体的 8 个角 + 6 个面的中心，方向分布均匀；长度是 0.25～1 之间定死的随机数
static const float3 kAoOffsets[14] = {
    float3( 0.216f,  0.216f,  0.216f), float3(-0.443f, -0.443f, -0.443f),
    float3(-0.419f,  0.419f,  0.419f), float3( 0.352f, -0.352f, -0.352f),
    float3( 0.238f,  0.238f, -0.238f), float3(-0.488f, -0.488f,  0.488f),
    float3(-0.494f,  0.494f, -0.494f), float3( 0.366f, -0.366f,  0.366f),
    float3(-0.629f,  0.000f,  0.000f), float3( 0.427f,  0.000f,  0.000f),
    float3( 0.000f, -0.252f,  0.000f), float3( 0.000f,  0.528f,  0.000f),
    float3( 0.000f,  0.000f, -0.689f), float3( 0.000f,  0.000f,  0.302f)
};

uint Pcg(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// 每个像素一个固定的"随机"单位向量：同一个像素每帧都一样（画面不闪），相邻像素之间没有规律
float3 RandomVec(int2 p)
{
    uint h = Pcg((uint)p.x + Pcg((uint)p.y));
    float3 v = float3(h & 1023u, (h >> 10) & 1023u, (h >> 20) & 1023u) / 1023.0f;
    return normalize(v * 2.0f - 1.0f + 1e-4f);
}

// 深度缓冲里的值 → 离相机多远（观察空间的 z）。近平面 1、远平面 1000，和 OnResize 里的 SetLens 一致
float ViewDepth(float depth)
{
    return 1000.0f / (1000.0f - 999.0f * depth);
}

// 模糊的 11 个权重：高斯，σ = 2.5，加起来等于 1。kBlurWeights[5] 是中间那个
static const float kBlurWeights[11] = {
    0.0222f, 0.0456f, 0.0798f, 0.1191f, 0.1514f, 0.1640f, 0.1514f, 0.1191f, 0.0798f, 0.0456f, 0.0222f
};

// 屏幕上第 pixel 个像素（全分辨率）那里的表面，在世界里的位置
float3 WorldPosAt(int2 pixel){
    uint width, height;
    gDepthMap.GetDimensions(width, height);
    float depth = gDepthMap.Load(int3(pixel, 0));
    float2 uv = (pixel + 0.5f) / float2(width, height); // 像素中心
    return WorldPosFromDepth(uv,depth);
}

// 挡住我的那个点离我 dist 远：算挡住多少（0～1） 输入 dist 是 r 到 p 的距离
float OcclusionFunction(float dist){
    float occlusion = 0.0f;
        if (dist > kAoEpsilon) // 比门槛远，才进来算
        {
            occlusion = saturate((kAoFadeEnd - dist) / (kAoFadeEnd - kAoFadeStart));
        }
        return occlusion;   // 没进 if 的（太近），返回的还是开头
}

float PS_Ssao(VertexOut pin): SV_Target{
    int2 pixel = int2(pin.PosH.xy) * 2;
    if(gDepthMap.Load(int3(pixel,0))>=1.0f)
        return 1.0f;
    float3 p = WorldPosAt(pixel);
    float3 n = normalize(gGBuffer1.Load(int3(pixel,0)).xyz);
    float3 randVec = RandomVec(int2(pin.PosH.xy));
    
    uint width,height;
    gDepthMap.GetDimensions(width,height);
    
    float occlusionSum = 0.0f;
    [loop]
    for(int i=0;i<kAoSamples;i++){
        float3 offset = reflect(kAoOffsets[i],randVec); // 14 个固定方向，用这个像素的随机向量翻一下
        float flip = sign(dot(offset, n));            // 和谁同侧才算在表面外面；不同侧就翻过来
        float3 q = p + flip * kAoRadius * offset;   
        
        float4 qH = mul(float4(q,1.0f),gPass.ViewProj); //世界 ->裁剪空间
        float2 qUV = qH.xy / qH.w * float2(0.5f, -0.5f) + 0.5f;  // 先变成 NDC（-1～1），再变成 uv
        if (any(qUV < 0.0f) || any(qUV >= 1.0f))
            continue; // 屏幕外：不知道那里有什么，当作没挡
        
        float3 r = WorldPosAt(int2(qUV* float2(width,height))); // q 那个像素上真正看得见的表面点
        float3 v = r - p;
        float dist = length(v);
        float dp = max(dot(n, v / max(dist, 1e-4f)), 0.0f); // r 在 p 正上方挡得最多，贴着表面的不算
        occlusionSum += dp * OcclusionFunction(dist);
        
    }
    float access = 1 - occlusionSum / kAoSamples; // 14 个点平均挡住了多少 → 没被挡住的比例
    return saturate(pow(access, kAoPower));
}

// 横、竖各一趟：编译时带宏 AO_BLUR_VERTICAL 的是竖的
float PS_SsaoBlur(VertexOut pin) : SV_Target{
    #ifdef AO_BLUR_VERTICAL
        const int2 dir = int2(0, 1); // 竖着走：每一步 x 加几、y 加几
    #else
        const int2 dir = int2(1, 0);
    #endif
    int2 aoPixel = int2(pin.PosH.xy);   // 遮蔽图上的像素
    float3 centerN = gGBuffer1.Load(int3(aoPixel*2,0)).xyz;
    float centerZ = ViewDepth(gDepthMap.Load(int3(aoPixel * 2, 0)));
    
    float sum = 0.0f;
    float weightSum = 0.0f;
    [unroll]
    for(int i=-5;i<=5;i++){
        int2 a = aoPixel + i * dir; // 左右（或上下）各 5 个邻居，i = 0 是自己
        float3 neighborN = gGBuffer1.Load(int3(a*2,0)).xyz;   // 邻居 a 对应的 G-Buffer 像素
        float neighborZ = ViewDepth(gDepthMap.Load(int3(a*2,0)));
        // 自己一定算；邻居要法线夹角小于约 37°（cos = 0.8）、远近差不到 0.2，才当成同一块表面
        if (i == 0 || (dot(neighborN, centerN) >= 0.8f && abs(neighborZ - centerZ) <= 0.2f))
        {
            sum += kBlurWeights[i+5] * gAoMap.Load(int3(a, 0)); // i 是 -5～5，数组下标是 0～10
            weightSum += kBlurWeights[i+5];
        }
    }
    return sum / weightSum;
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
    // 深度缓冲里的值和距离不成正比（近处变得快，远处全挤在 1 附近），换回观察空间的 z 再显示
    c = depth >= 1.0f ? 1.0f : saturate(ViewDepth(depth) / 150.0f);
#elif GBUFFER_VIEW == 8
    // 太阳的阴影因子（白 = 亮，黑 = 影子里）：不算光、不加环境光，看痤疮、悬浮、PCF 的边最清楚
    if (depth >= 1.0f)
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    c = CalcShadowFactor(gPass, WorldPosFromDepth(pin.UV, depth), normal);
#elif GBUFFER_VIEW == 7
    // SSAO 的遮蔽图（半分辨率，所以除以 2；不过滤，看到的就是存的值）：白 = 没被挡，黑 = 挡住了
    c = gAoMap.Load(int3(pixel / 2, 0));
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


