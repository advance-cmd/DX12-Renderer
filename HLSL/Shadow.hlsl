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
//3x3 PCF
float PCF(int i,float2 uv,float depth){
    uint w,h,n;
    gShadowMap.GetDimensions(w,h,n);    //纹理数组：宽，高，几层
    float dx = 1.0f/(float)w;
    const float2 offsets[9] = {
        float2(-dx, -dx), float2(0, -dx), float2(dx, -dx),
        float2(-dx, 0), float2(0, 0), float2(dx, 0),
        float2(-dx, dx), float2(0, dx), float2(dx, dx)
    };
    float percentLit = 0.0f;    //这个点周围的采样范围里，有多大比例照得到太阳
    [unroll]
    for(int k=0;k<9;k++){
        percentLit +=gShadowMap.SampleCmpLevelZero(gsamShadow,float3(uv + offsets[k],i),depth);
    }
    return percentLit/9.0f;
}
// 第 i 级阴影图里，这个点被挡了多少（normal 第②步才用）
float CascadeShadow(PassConstants pass,int i,float3 posW,float3 normal){
    float4 sp =mul(float4(posW,1.0f),pass.ShadowTransforms[i]);    //正交投影
    return PCF(i,sp.xy,sp.z);
}
// 返回 0～1：1 = 全亮，0 = 全在影子里
float CalcShadowFactor(PassConstants pass,float3 posW,float3 normal){
    float viewZ = ViewZ(pass,posW);
    int i =CascadeIndex(pass,viewZ);
    if (viewZ > pass.CascadeSplits[i])
        return 1.0f;
    return CascadeShadow(pass, i, posW, normal);
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
    return kCascadeColors[i];
}