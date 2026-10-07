// 色调映射：HDR 场景纹理 × 曝光 → 色调曲线压到 [0, 1] → sRGB 编码 → 后台缓冲
Texture2D<float4> gHdrMap : register(t0);
Texture2D<float4> gBloomMap : register(t1);  // Bloom 的 mip 0（半分辨率），和 t0 挨着放在堆第 19 格
SamplerState gLinearClamp : register(s0);    // 根签名里的静态采样器：双线性 + 夹边

// 根常量（C++ 里 SetGraphicsRoot32BitConstants 写进来的三个 32 位数，顺序要和 C++ 的 struct 一样）
struct TonemapSettings
{
    float Exposure;       // 2^EV
    uint Curve;           // 0 截断 / 1 Reinhard / 2 ACES / 3 AgX
    float BloomIntensity; // Bloom 加回去的比例；Bloom 关掉时 C++ 传 0
};
ConstantBuffer<TonemapSettings> gTonemap : register(b0);

// Reinhard：x / (1 + x)。0.18 → 0.15，1 → 0.5，无论多亮都到不了 1
float3 Reinhard(float3 x)
{
    return x / (1.0f + x);
}

// ACES 电影曲线的拟合（Narkowicz 2015）：暗部压一点、中间调提对比、亮部平滑地压向 1。
float3 ACESFitted(float3 x)
{
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// AgX（Blender 4.0 起的默认）：转到 AgX 颜色空间 → 取 log2 映射到 [0, 1] → 过一条 S 形曲线 → 转回来。
// 很亮的饱和色会往白色走，比如太阳下的橙色不会变成一块死橙，也不偏色。常数照抄
float3 AgXContrast(float3 x)
{
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;
}

float3 AgX(float3 c)
{
    const float3x3 toAgX = {
        0.842479062253094f, 0.0423282422610123f, 0.0423756549057051f,
        0.0784335999999992f, 0.878468636469772f, 0.0784336f,
        0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f};
    const float3x3 fromAgX = {
        1.19687900512017f, -0.0528968517574562f, -0.0529716355144438f,
        -0.0980208811401368f, 1.15190312990417f, -0.0980434501171241f,
        -0.0990297440797205f, -0.0989611768448433f, 1.15107367264116f};
    const float minEV = -12.47393f, maxEV = 4.026069f;
    c = mul(c, toAgX);
    c = clamp(log2(max(c, 1e-10f)), minEV, maxEV);
    c = (c - minEV) / (maxEV - minEV);
    c = AgXContrast(c);              // 出来的已经是"显示用"的编码值
    c = mul(c, fromAgX);
    return pow(saturate(c), 2.2f);   // 换回线性，后面统一做 sRGB 编码
}

struct VertexOut
{
    float4 PosH : SV_Position;
    float2 UV : TEXCOORD; // 0～1 的纹理坐标：Bloom 是半分辨率，不能像 HDR 那样按像素坐标 Load，要按 UV 采样
};

// 不用顶点缓冲：DrawInstanced(3) 时 id = 0、1、2
VertexOut VS(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2); // id 0、1、2 → uv (0,0)、(2,0)、(0,2)
    VertexOut vout;
    vout.PosH = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    vout.UV = uv;
    return vout;
}

// 线性 → sRGB 编码
float3 LinearToSRGB(float3 c)
{
    return select(c <= 0.0031308f, 12.92f * c, 1.055f * pow(c, 1.0f / 2.4f) - 0.055f);
}

float4 PS(VertexOut pin) : SV_Target
{
    // 曝光乘在曲线之前：先决定场景里多亮算"中间调"，再由曲线把亮部平滑地压下去
    float3 hdr = gHdrMap.Load(int3(pin.PosH.xy, 0)).rgb;
    hdr += gBloomMap.SampleLevel(gLinearClamp,pin.UV,0).rgb * gTonemap.BloomIntensity;
    hdr *= gTonemap.Exposure;
    float3 ldr;
    if (gTonemap.Curve == 1)      ldr = Reinhard(hdr);
    else if (gTonemap.Curve == 2) ldr = ACESFitted(hdr);
    else if (gTonemap.Curve == 3) ldr = AgX(hdr);
    else                          ldr = saturate(hdr);
    return float4(LinearToSRGB(ldr), 1.0f);
}