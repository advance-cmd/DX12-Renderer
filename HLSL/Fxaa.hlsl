// FXAA：色调映射之后的 LDR 图 → 后台缓冲。第①步先原样输出，第②步再换成 FXAA
Texture2D<float4> gLdrMap : register(t0);
SamplerState gLinearClamp : register(s0);   // 双线性 + 夹边（根签名里的静态采样器）

struct FxaaSettings
{
    float2 RcpFrame;     // 一个像素在 uv 里有多宽、多高
    float Subpix;
    float EdgeThreshold;
    uint Debug;
};

ConstantBuffer<FxaaSettings> gFxaa : register(b0);

struct VertexOut
{
    float4 PosH :SV_Position;
};

// 不用顶点缓冲：DrawInstanced(3) 时 id = 0、1、2
VertexOut VS(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2); // id 0、1、2 → uv (0,0)、(2,0)、(0,2)
    VertexOut vout;
    vout.PosH = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return vout;
}

static const float kEdgeThresholdMin = 0.0833f; // 门槛的下限：再暗的地方，对比度也得 ≥ 这个数才算边
static const int kSteps = 5;
static const float kStepSize[kSteps] = {1.0f, 1.5f, 2.0f, 4.0f, 12.0f}; // 沿边每一步走几个像素，越走越大

//亮度：人眼对绿最敏感
float Luma(float3 c){
    return dot(c, float3(0.299f, 0.587f, 0.114f));
}

float LumaAt(float2 uv)
{
    return Luma(gLdrMap.SampleLevel(gLinearClamp, uv, 0).rgb);
}

float4 PS(VertexOut pin) : SV_Target
{
    const float2 uv = pin.PosH.xy * gFxaa.RcpFrame; // 像素中心的 uv：PosH.xy 是像素坐标（带 0.5），换成 0～1
    const float3 rgbM = gLdrMap.SampleLevel(gLinearClamp, uv, 0).rgb;
    const float m = Luma(rgbM);
    const float n = Luma(gLdrMap.SampleLevel(gLinearClamp, uv, 0, int2(0, -1)).rgb); // 上（y 往下是正）
    const float s = Luma(gLdrMap.SampleLevel(gLinearClamp, uv, 0, int2(0, 1)).rgb); // 下
    const float w = Luma(gLdrMap.SampleLevel(gLinearClamp, uv, 0, int2(-1, 0)).rgb); // 左
    const float e = Luma(gLdrMap.SampleLevel(gLinearClamp, uv, 0, int2(1, 0)).rgb); // 右
    
    // ① 对比度：自己和上下左右五个里，最亮 − 最暗。不够门槛 → 不是边，原样输出（调试视图里是黑的）
    const float maxL =max(max(max(n,s),max(w,e)),m);
    const float minL =min(min(min(n,s),min(w,e)),m);
    const float range = maxL - minL;
    if (range < max(kEdgeThresholdMin, maxL * gFxaa.EdgeThreshold)){
        return float4(gFxaa.Debug!=0 ? float3(0,0,0):rgbM,1.0f);
    }
    const float nw = Luma(gLdrMap.SampleLevel(gLinearClamp, uv, 0, int2(-1, -1)).rgb);
    const float ne = Luma(gLdrMap.SampleLevel(gLinearClamp, uv, 0, int2(1, -1)).rgb);
    const float sw = Luma(gLdrMap.SampleLevel(gLinearClamp, uv, 0, int2(-1, 1)).rgb);
    const float se = Luma(gLdrMap.SampleLevel(gLinearClamp, uv, 0, int2(1, 1)).rgb);
    
    // ② 横边还是竖边：上下方向的二阶差分（上 + 下 − 2×中）大 = 上下变化剧烈 = 横着的边
    //    左、中、右三列各算一次，中间那列权重 2
    const float edgeHorz = abs(-2 * w + nw + sw) + 2 * abs(-2 * m + n + s) + abs(-2 * e + ne + se);
    const float edgeVert = abs(-2 * n + nw + ne) + 2 * abs(-2*m + w + e)+ abs(-2 * s +sw + se); // 把上面这行"转 90°"：左右方向的二阶差分，上、中、下三行，中间那行权重 2
    const bool horz = edgeHorz >= edgeVert;  //这个边判定为 横边/竖边 
    
    // ③ 次像素混合量：3×3 加权平均（上下左右权重 2，四个角权重 1，一共 12）和自己差多少，除以对比度 → 0～1
    const float avg = (2 * (n + s + w + e) + nw + ne + sw + se) / 12.0f;
    const float subC = saturate(abs(avg - m)/range);    // 自己和周围的平均差多少，占这块对比度的几成
    const float subS = (-2 * subC + 3)*subC * subC; // smoothstep：两头平、中间陡   3x² − 2x³小的压得更小，大的推得更大
    const float subH = subS * subS * gFxaa.Subpix;  // 再平方一次，乘上面板的"次像素混合"（默认 0.75）
    
    // ④ 往边的哪一侧混：横边看上下（n、s），竖边看左右（w、e），选和自己差得多的那一侧
    const float l1 = horz ? n : w;  // "上 / 左"那一侧
    const float l2 = horz ? s : e;  // "下 / 右"那一侧
    const bool pick1 = abs(l1 - m) >= abs(l2 - m);
    const float gradient = max(abs(l1 - m), abs(l2 - m));
    float stepLen = horz ? gFxaa.RcpFrame.y : gFxaa.RcpFrame.x;
    if (pick1)
        stepLen = -stepLen; // 往上 / 往左是负方向
    const float edgeLuma = ((pick1 ? l1 : l2) + m) * 0.5f; // 边上的平均亮度：自己和选中那一侧邻居的中间值
    
    // ⑤ 沿边往两头走。采样点放在"自己和那一侧邻居"的交界线上（挪半个像素）：
    //    双线性采在两个像素正中间 = 两行（或两列）各占一半，正好是"边上"的亮度
    float2 posB = uv;
    if(horz)
        posB.y +=stepLen*0.5;
    else
        posB.x +=stepLen*0.5;
    const float2 offNP = horz ? float2(gFxaa.RcpFrame.x,0):float2(0,gFxaa.RcpFrame.y);  // 沿着边走一个像素：横边沿 x 走，竖边沿 y 走
    const float gScaled = gradient * 0.25f; // 离 edgeLuma 超过这么多 = 台阶拐了，这一头到了
    float2 posN = posB - offNP * kStepSize[0];
    float2 posP = posB + offNP * kStepSize[0];
    float endN = LumaAt(posN) - edgeLuma;
    float endP = LumaAt(posP) - edgeLuma;
    bool doneN = abs(endN) >= gScaled;
    bool doneP = abs(endP) >= gScaled;
    [unroll]
    for(int k=1;k<kSteps;++k){
        if (!doneN)
            posN -= offNP * kStepSize[k];
        if (!doneP)
            posP += offNP * kStepSize[k];
        if(k == kSteps -1)
            break;  // 最后一步（12 像素）只挪不采：还没找到，就当那头在更远的地方
        if (!doneN)
        {
            endN = LumaAt(posN) - edgeLuma;
            doneN = abs(endN) >= gScaled;
        }
        if(!doneP){
        endP = LumaAt(posP) - edgeLuma;
        doneP = abs(endP) >=gScaled;
        }
    }
    
    // ⑥ 离哪头近：混合比例 = 0.5 − 近的那头的距离 / 这段边的总长
    const float dstN = horz ? uv.x - posN.x : uv.y - posN.y;
    const float dstP = horz ? posP.x - uv.x : posP.y - uv.y;
    // 台阶方向对不对：自己比边亮（m > edgeLuma）时，近的那头应该是变暗的（end < 0），反过来也一样。
    // 不对 = 这一级台阶是往另一边拐的，这个像素不混，留给另一侧那排像素去混（小模型里的"不混（0）"）
    const bool mBelow = m <edgeLuma;
    const bool goodN = (endN < 0)!=mBelow;
    const bool goodP = (endP <0)!=mBelow;
    const bool nearN = dstN < dstP;
    const float pixelOffset = 0.5f - min(dstN, dstP) / (dstN + dstP);
    const float spanOffset = (nearN ? goodN : goodP) ? pixelOffset : 0.0f;
    const float finalOffset = max(subH,spanOffset);
    if (gFxaa.Debug != 0) // 调试视图（AI 给的，照抄）：红 = 横边，蓝 = 竖边，越亮混得越多
        return float4((horz ? float3(1.0f, 0.25f, 0.2f) : float3(0.2f, 0.5f, 1.0f)) * clamp(finalOffset * 2, 0.15f, 1.0f), 1.0f);
    
    // ⑦ 垂直于边挪 finalOffset 个像素再采一次：双线性 = 自己 × (1 − finalOffset) + 那一侧邻居 × finalOffset
    float2 uvF = uv;
    if (horz)
        uvF.y += stepLen * finalOffset;
    else
        uvF.x += stepLen * finalOffset;
    return float4(gLdrMap.SampleLevel(gLinearClamp, uvF, 0).rgb, 1.0f);
}