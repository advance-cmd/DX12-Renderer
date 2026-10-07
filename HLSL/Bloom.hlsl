// Bloom：在半分辨率的 mip 链上，先逐级缩小（每级更糊），再逐级放大叠加回来
Texture2D<float4> gSrc : register(t0);  // 这一趟读的那一级（第一趟读 HDR 场景纹理）
SamplerState gLinearClamp : register(s0);

struct BloomSettings
{
    float2 TexelSize; // 读的那一级：1 / 宽高
    float Threshold;  // 亮度超过它才参与 Bloom（场景亮度，曝光之前）
    float Knee;       // 阈值附近的软过渡宽度
    float Radius;     // 放大时帐篷滤波的半径（单位：读的那一级的像素）
    uint Karis;       // 第一级降采样用 Karis 平均压掉"萤火虫"
};
ConstantBuffer<BloomSettings> gBloom : register(b0);

struct VertexOut{
    float4 PosH : SV_Position;
    float2 UV :TEXCOORD;
};

// 和 Tonemap 的 VS 一样，多输出一个 UV：uv 本身就是 0～1 的纹理坐标
VertexOut VS(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    VertexOut vout;
    vout.PosH = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    vout.UV = uv;
    return vout;
}

float3 S(float2 uv){
    return gSrc.SampleLevel(gLinearClamp, uv, 0).rgb;
}

float KarisWeight(float3 c){
    return 1.0f / (1.0f + dot(c, float3(0.2126f, 0.7152f, 0.0722f)));
}

// 13 点降采样：中间 4 点一组、四个角各 4 点一组（共 5 组）。
// 每个采样点落在 4 个纹素中间，双线性一次就平均 4 个，13 次采样覆盖 6×6 个源纹素，镜头动的时候亮线不闪
float3 Downsample13(float2 uv, bool karis)
{
    float2 t = gBloom.TexelSize;
    float3 a = S(uv + t * float2(-2, -2)), b = S(uv + t * float2(0, -2)), c = S(uv + t * float2(2, -2));
    float3 d = S(uv + t * float2(-1, -1)), e = S(uv + t * float2(1, -1));
    float3 f = S(uv + t * float2(-2, 0)), g = S(uv), h = S(uv + t * float2(2, 0));
    float3 i = S(uv + t * float2(-1, 1)), j = S(uv + t * float2(1, 1));
    float3 k = S(uv + t * float2(-2, 2)), l = S(uv + t * float2(0, 2)), m = S(uv + t * float2(2, 2));
    float3 g0 = (d + e + i + j) * 0.25f;                                   // 中间一组
    float3 g1 = (a + b + f + g) * 0.25f, g2 = (b + c + g + h) * 0.25f;     // 四个角的组
    float3 g3 = (f + g + k + l) * 0.25f, g4 = (g + h + l + m) * 0.25f;
    if (!karis)
        return g0 * 0.5f + (g1 + g2 + g3 + g4) * 0.125f;   // 空1：中间一组权重 0.5，四个角的组各 0.125
    // Karis 平均：每组按 1 / (1 + 亮度) 加权，一个特别亮的像素（水面上的太阳亮点）不会一家独大，光晕不闪
    float w0 = 0.5f * KarisWeight(g0), w1 = 0.125f * KarisWeight(g1), w2 = 0.125f * KarisWeight(g2);
    float w3 = 0.125f * KarisWeight(g3), w4 = 0.125f * KarisWeight(g4);
    return (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4);
}

// 亮部提取（软阈值）：亮度 ≤ 阈值 - 膝 的不要，≥ 阈值 + 膝 的全要，中间二次曲线过渡，免得出现硬边
float3 Prefilter(float3 c)
{
    float br = max(c.r, max(c.g, c.b));
    float rq = clamp(br - gBloom.Threshold + gBloom.Knee, 0.0f, 2.0f * gBloom.Knee);
    rq = rq * rq / (4.0f * gBloom.Knee + 1e-5f);
    float w = max(rq, br - gBloom.Threshold) / max(br, 1e-5f);
    return c * w;
}

// 第一趟：HDR 场景纹理（全分辨率）→ mip 0（半分辨率），顺便做亮部提取
float4 PS_Prefilter(VertexOut pin) : SV_Target
{
    return float4(Prefilter(Downsample13(pin.UV, gBloom.Karis != 0)), 1.0f);
}

// 后面几趟：mip i-1 → mip i
float4 PS_Downsample(VertexOut pin) : SV_Target
{
    return float4(Downsample13(pin.UV, false), 1.0f);
}

// 放大：mip i+1 用 3×3 帐篷滤波读进来（中心 4、边 2、角 1），PSO 的加法混合把它加到 mip i 原来的内容上
float4 PS_Upsample(VertexOut pin) : SV_Target
{
    float2 t = gBloom.TexelSize * gBloom.Radius;
    float3 r = S(pin.UV) * 4.0f;
    r += (S(pin.UV + float2(-t.x, 0)) + S(pin.UV + float2(t.x, 0)) + S(pin.UV + float2(0, -t.y)) + S(pin.UV + float2(0, t.y))) * 2.0f;
    r += S(pin.UV + float2(-t.x, -t.y)) + S(pin.UV + float2(t.x, -t.y)) + S(pin.UV + float2(-t.x, t.y)) + S(pin.UV + float2(t.x, t.y));
    return float4(r / 16, 1.0f); 
}