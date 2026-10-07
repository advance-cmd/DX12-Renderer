// 色调映射：把 HDR 场景纹理压到后台缓冲。第 ① 步只做两件事：截到 [0, 1]，再做 sRGB 编码
Texture2D<float4> gHdrMap : register(t0);

struct VertexOut
{
    float4 PosH : SV_Position;
};

// 不用顶点缓冲：DrawInstanced(3) 时 id = 0、1、2
VertexOut VS(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2); // id 0、1、2 → uv (0,0)、(2,0)、(0,2)
    VertexOut vout;
    vout.PosH = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return vout;
}

// 线性 → sRGB 编码
float3 LinearToSRGB(float3 c)
{
    return select(c <= 0.0031308f, 12.92f * c, 1.055f * pow(c, 1.0f / 2.4f) - 0.055f);
}

float4 PS(VertexOut pin) : SV_Target
{
    float3 hdr = gHdrMap.Load(int3(pin.PosH.xy, 0)).rgb;
    float3 ldr = saturate(hdr);                  // 第 ② 步换成曝光 + 色调曲线
    return float4(LinearToSRGB(ldr), 1.0f);
}