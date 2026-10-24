// TAA（s110）：这一帧（带抖动、有锯齿）和上一帧的结果按速度对齐后混合，写进另一张历史图
// 根签名 TAA：t0～t3 各一张表，b1 = Pass 常量（主 Pass 那份），b0 = 4 个根常量，s0 = 双线性 + 夹边
Texture2D<float4> gCurrent : register(t0);  // 这一帧的 HDR（③ 画完的场景）
Texture2D<float4> gHistory : register(t1);  // 上一帧的 TAA 结果（另一张历史图）
Texture2D<float2> gVelocity : register(t2); // 速度缓冲：这一帧的 uv − 上一帧的 uv
Texture2D<float> gDepth : register(t3);     // 深度：天空（深度 = 1）的速度要靠它反推
SamplerState gLinearClamp : register(s0);   // 历史图在两个像素之间取：双线性

// 只用到 Pass 常量开头的 4 个矩阵：顺序必须和 LightingUtil.hlsl 的 PassConstants 开头一样（常量缓冲按字节偏移读）
struct TaaPass
{
    float4x4 ViewProj;
    float4x4 InvViewProj;
    float4x4 ViewProjNoJitter;
    float4x4 PrevViewProjNoJitter;
};
ConstantBuffer<TaaPass> gPass : register(b1);

// 根常量：顺序要和 C++ 里的 struct 一样
struct TaaSettings
{
    float2 RcpFrame; // 一个像素在 uv 里有多宽、多高 = (1 / 宽, 1 / 高)
    float Alpha;     // 这一帧占多少（0.1 = 新的 10%、历史 90%）
    uint Reset;      // 1 = 历史作废（第一帧、改尺寸、刚打开 TAA）：直接输出这一帧
    uint Clamp;      // 1 = 邻域裁剪（第④步）：历史只能在这一帧 3×3 邻居的颜色范围里
};
ConstantBuffer<TaaSettings> gTaa : register(b0);

struct VertexOut
{
    float4 PosH : SV_Position;
};

// 全屏三角形（和 Tonemap.hlsl、Fxaa.hlsl 的 VS 一样）
VertexOut VS(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    VertexOut vout;
    vout.PosH = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    const int2 pixel = int2(pin.PosH.xy);
    const float2 uv = pin.PosH.xy * gTaa.RcpFrame;  //像素中心的uv
    const float3 current = gCurrent.Load(int3(pixel,0)).rgb;
    if(gTaa.Reset != 0){
        return float4(current,1.0f);    //历史作废：这一帧就是新的起点
    }
    
    // ① 这个像素上一帧在哪
    float2 velocity = gVelocity.Load(int3(pixel,0));
    if(gDepth.Load(int3(pixel,0))>=1.0f){   //天空
        float4 posW = mul(float4(uv.x*2.0f-1.0f,1.0f-uv.y*2.0f,1.0f,1.0f),gPass.InvViewProj);   //uv -> NDC -> World
        posW /= posW.w; //撤销透视除法
        const float4 cur = mul(posW,gPass.ViewProjNoJitter);
        const float4 prev = mul(posW,gPass.PrevViewProjNoJitter);
        velocity = (cur.xy / cur.w - prev.xy / prev.w) * float2(0.5f, -0.5f); // 和 MotionVector 一样
    }
    const float2 prevUV = uv - velocity;
    if(any(prevUV < 0.0f) || any(prevUV > 1.0f))
        return float4(current,1.0f);    // 上一帧这里在屏幕外：没有历史可用
    
    // ② 取上一帧的结果（双线性），和这一帧按 α 混合
    float3 history = gHistory.SampleLevel(gLinearClamp, prevUV, 0).rgb;
    
    // ③ 邻域裁剪：历史只能在这一帧 3×3 邻居的颜色范围里
    if (gTaa.Clamp != 0)
    {
        float3 cMin = current, cMax = current;
        [unroll]
        for (int y = -1; y <= 1; y++)
        {
            [unroll]
            for (int x = -1; x <= 1; x++)
            {
                const float3 c = gCurrent.Load(int3(pixel + int2(x, y), 0)).rgb; // 出了屏幕 Load 返回 0，只会让范围宽一点
                cMin = min(cMin,c);
                cMax = max(cMax,c);
            }
    }
    history = clamp(history,cMin,cMax);
    }
    return float4(lerp(history, current, gTaa.Alpha), 1.0f); // lerp(a, b, t) = a·(1 − t) + b·t
}
