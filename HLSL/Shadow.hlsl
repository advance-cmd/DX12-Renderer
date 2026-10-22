// 级联阴影（CSM）：选级联 → 法线偏移 → 投到这一级的阴影图 → PCF 或 PCSS；交界处两级混合
// LightingUtil.hlsl 在声明完 PassConstants、gShadowMap（t1）、gsamShadow（s1）之后 include 本文件
// 用到 gPass 的函数都把它当参数传进来（和 AmbientLighting 一样）：gPass 是各个 .hlsl 自己声明的

// ---- 数据（AI 写的）----
// 16 个点的泊松圆盘：单位圆里撒得比较均匀、又不成网格的 16 个点（第④步 PCSS 的找遮挡物和可变核 PCF 共用）
static const float2 kPoisson16[16] = {
    float2(-0.94201624f, -0.39906216f), float2(0.94558609f, -0.76890725f),
    float2(-0.09418410f, -0.92938870f), float2(0.34495938f, 0.29387760f),
    float2(-0.91588581f, 0.45771432f), float2(-0.81544232f, -0.87912464f),
    float2(-0.38277543f, 0.27676845f), float2(0.97484398f, 0.75648379f),
    float2(0.44323325f, -0.97511554f), float2(0.53742981f, -0.47373420f),
    float2(-0.26496911f, -0.41893023f), float2(0.79197514f, 0.19090188f),
    float2(-0.24188840f, 0.99706507f), float2(-0.81409955f, 0.91437590f),
    float2(0.19984126f, 0.78641367f), float2(0.14383161f, -0.14100790f)
};
// 面板"级联上色"：第 0～3 级 红、绿、蓝、黄
static const float3 kCascadeColors[4] = {
    float3(1.0f, 0.35f, 0.35f), float3(0.35f, 1.0f, 0.35f), float3(0.35f, 0.55f, 1.0f), float3(1.0f, 1.0f, 0.35f)
};
// PCSS 找遮挡物的范围：挡光的东西最多离接收面这么远（世界单位）。再远的遮挡物找不到，半影就偏小
static const float kPcssMaxOccluderDist = 20.0f;

// ---- 下面是第①步写的函数：选级联 + 3×3 PCF。第②～④步（偏移、过渡带、PCSS）接着在这里改 ----
// 观察空间的 z（离相机多远，沿视线方向）
float ViewZ(PassConstants pass, float3 posW)
{
    return mul(float4(posW,1),pass.ViewProj).w;
}

// 这个距离归第几级管：从第 0 级往后找，第一个"远端比它远"的级联
int CascadeIndex(PassConstants pass, float viewZ)
{
    int i = 0;
    [loop]
    while (i < pass.CascadeCount - 1 && viewZ > pass.CascadeSplits[i])
        ++i;
    return i;
}
// PCF：(2r+1)×(2r+1) 个点，每个点硬件再做一次 2×2 比较 + 双线性
float PCF(int i, float2 uv, float depth, int radius)
{
    uint w, h, n;
    gShadowMap.GetDimensions(w, h, n);
    float dx = 1.0f / (float)w;
    float sum = 0.0f;
    [loop]
    for (int y = -radius; y <= radius; ++y)
    {
        [loop]
        for (int x = -radius; x <= radius; ++x)
            sum += gShadowMap.SampleCmpLevelZero(gsamShadow, float3(uv + dx*float2(x,y), i), depth); // 以前的 offsets[k] 是怎么写出来的？
    }
    float side = 2*radius+1; // 一行几个点
    return sum / (side*side);
}
// PCSS 软阴影：① 找遮挡物 → ② 算半影多宽 → ③ 按半影宽度做 PCF 最终返回阴影因子
float PCSS(PassConstants pass,int i,float2 uv,float depth){
    uint w,h,n;
    gShadowMap.GetDimensions(w,h,n);
    float worldPerUV = pass.CascadeTexel[i]*w;  // 这一级阴影图整张有多宽（世界单位）
    
    // ① 找遮挡物：在周围找比自己离太阳近的纹素，求它们的平均深度
    //    找多大一圈：最远的遮挡物（kPcssMaxOccluderDist）能造成的半影，换成 uv
    float searchUV = (kPcssMaxOccluderDist * pass.LightTan)/worldPerUV;
    float blockerSum = 0.0f; // 找到的遮挡物深度加起来，最后除以个数 = 平均遮挡物深度
    float blockers = 0.0f;   // 找到了几个遮挡物（用 float：后面直接当除数）；一个都没有 = 周围没东西挡光
    [loop]
    for(int k = 0;k<16;k++){
        float2 suv = uv + kPoisson16[k]*searchUV;
        int2 texel = clamp(suv*w,0,int(w)-1);// uv → 第几个纹素；Load 越界读出来是 0（= 最近），会被当成遮挡物  阴影图上第几列、第几行的那个像素    Load 不认 uv，只认这种整数坐标
        float d = gShadowMap.Load(int4(texel,i,0)).r;
        if(d<depth){
            blockerSum +=d;
            blockers +=1.0f;
        }
    }
    if(blockers == 0.0f){
        return 1.0f;
    }
    float avgBlocker = blockerSum / blockers;   //平均遮挡深度
    
    // ② 半影半宽（世界单位）：遮挡物到接收面的距离 × tan(太阳角半径)
    //    深度差是 0～1 里的数，乘 ShadowDepthRange 才是世界单位
    float penumbraWorld = (depth - avgBlocker) * pass.LightTan * pass.ShadowDepthRange;
    float filterUV = max(penumbraWorld/worldPerUV,1.0f/w); // 至少1个纹素
    
    // ③ 可变核 PCF：还是这 16 个点，半径换成半影宽度
    float sum = 0.0f;
    [loop]
    for (int k2 = 0; k2 < 16; ++k2)
    sum += gShadowMap.SampleCmpLevelZero(gsamShadow, float3(uv + filterUV*kPoisson16[k2], i), depth);
    return sum / 16.0f;
}
// 第 i 级阴影图里，这个点被挡了多少（normal 第②步才用）
float CascadeShadow(PassConstants pass,int i,float3 posW,float3 normal){
    // 法线偏移：沿法线往外挪几个纹素再去查。pass.NormalOffset 的单位是"这一级的纹素"
    float3 p = posW + pass.NormalOffset*normal*pass.CascadeTexel[i];
    float4 sp = mul(float4(p, 1.0f), pass.ShadowTransforms[i]); // 正交投影：w = 1，不用除
    if (pass.Pcss != 0) // 面板勾了"PCSS 软阴影"：核大小按半影算；没勾：固定大小的 PCF
        return PCSS(pass, i, sp.xy, sp.z);
    return PCF(i, sp.xy, sp.z, pass.PcfRadius);
}

// 过渡带里的混合系数 t：过渡带起点 = 1，这一级远端 = 0，中间按距离线性变（再往近 > 1 = 不在过渡带里）
float CascadeBlendT(PassConstants pass, int i, float viewZ)
{
    float farZ = pass.CascadeSplits[i];                    // 这一级的远端
    float nearZ = i == 0 ? 0.0f : pass.CascadeSplits[i-1];   // 这一级的近端 = 上一级的远端
    float band = (farZ - nearZ)*pass.CascadeBlend;                    // 过渡带多长：这一级长度 × 面板"过渡带"
    return band > 0.0f ? (farZ - viewZ)/band : 1.0f;     // 面板拖到 0 = 不过渡，别除以 0
}

// 返回 0～1：1 = 全亮，0 = 全在影子里
float CalcShadowFactor(PassConstants pass,float3 posW,float3 normal){
    float viewZ = ViewZ(pass,posW);
    int i =CascadeIndex(pass,viewZ);
    if (viewZ > pass.CascadeSplits[i])
        return 1.0f;
    float s = CascadeShadow(pass, i, posW, normal);
    // 过渡带：这一级最后那一段，和下一级的结果混合；最后一级没有下一级，就和"全亮"混合（阴影慢慢淡出）
    float t = CascadeBlendT(pass, i, viewZ);
    [branch] // 不在过渡带里的像素整段跳过，不多采一次
    if (t < 1)
    {
        float next = (i+1)<pass.CascadeCount ? CascadeShadow(pass, i + 1, posW, normal) : 1;
        s = lerp(next, s, t); // t = 1 → 全用这一级，t = 0 → 全用下一级
    }
    return s;
}
// 面板"级联上色"（AI 写的，照抄）：每级一种颜色；关掉时乘 1
float3 CascadeDebugTint(PassConstants pass, float3 posW)
{
    if (pass.ShadowDebug == 0)
    return 1.0f;
    float viewZ = ViewZ(pass, posW);
    int i = CascadeIndex(pass, viewZ);
    if (viewZ > pass.CascadeSplits[i])
    return 1.0f;
    float t = saturate(CascadeBlendT(pass, i, viewZ));
    float3 next = i + 1 < pass.CascadeCount ? kCascadeColors[i + 1] : 1.0f;
    return lerp(next, kCascadeColors[i], t);
}