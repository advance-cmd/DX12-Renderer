// IBL 预计算：三个计算着色器，启动后第一帧（和热重载之后）各跑一次，结果存成三张图，之后每帧只查不写
//   CS_Irradiance  天空图 → 辐照度图（32×32 立方体）：漫反射用，"法线朝这边的半球，平均来了多少光"
//   CS_Prefilter   天空图 → 预滤波环境贴图（256×256 立方体，5 级 mip）：镜面用，第 i 级按粗糙度 i/4 模糊
//   CS_BrdfLut     不读天空图 → BRDF LUT（256×256，两个通道）：镜面的另一半，只和 NoV、粗糙度有关
// 根签名（IBL 专用，计算）：b0 = 1 个根常量（预滤波这一级的粗糙度），t0 = 天空图，u0 = 这一趟要写的那张图（那一级 mip），s0 = 三线性
#include "LightingUtil.hlsl"   // PI、MinRoughness、D_GGX、V_SmithGGXCorrelated；s0 就是那里的 gsamLinearWrap

cbuffer IblSettings : register(b0)
{
    float gRoughness; // 预滤波：这一级 mip 对应的粗糙度（感知粗糙度：0、0.25、0.5、0.75、1）
};

TextureCube gEnvMap : register(t0);               // 源：天空图（1024×1024，11 级 mip）
RWTexture2DArray<float4> gOutCube : register(u0); // 立方体的一级 mip，看成 6 张图的数组：第三个下标 = 第几个面
RWTexture2D<float2> gOutLut : register(u0);       // BRDF LUT（只有 CS_BrdfLut 用，和上面那个不会出现在同一个着色器里）

// ---- 数据和小工具（AI 写的） ----

// 立方体第 face 个面上的一点 → 方向（没归一化）。uv 在 -1～1，左上角是 (-1, -1)，v 向下（和贴图的行一样）
// 6 个面的顺序是 D3D 规定的：+X、-X、+Y、-Y、+Z、-Z
float3 CubeDir(uint face, float2 uv)
{
    switch (face)
    {
    case 0: return float3(1.0f, -uv.y, -uv.x);   // +X
    case 1: return float3(-1.0f, -uv.y, uv.x);   // -X
    case 2: return float3(uv.x, 1.0f, uv.y);     // +Y
    case 3: return float3(uv.x, -1.0f, -uv.y);   // -Y
    case 4: return float3(uv.x, -uv.y, 1.0f);    // +Z
    default: return float3(-uv.x, -uv.y, -1.0f); // -Z
    }
}

// Hammersley 点集：n 个点里的第 i 个，两个坐标都在 0～1，比随机数铺得均匀
// reversebits(i)：把 32 位二进制倒过来（第 0 位换到第 31 位……），再除以 2³² 变成 0～1
float2 Hammersley(uint i, uint n)
{
    return float2((float)i / n, reversebits(i) * 2.3283064365386963e-10f);
}

static const uint kIrrPhiSteps = 128;  // 辐照度：绕法线一圈分 128 份
static const uint kIrrThetaSteps = 32; // 从法线到地平线（90°）分 32 份，每份 2.8°
static const float kIrrSrcMip = 5.0f;  // 每份 2.8°，正好是天空图第 5 级 mip（32×32）一个像素的大小
static const uint kPrefilterSamples = 1024;
static const uint kLutSamples = 1024;

// 这个线程管的像素（id.xy，第 id.z 个面）中心的方向，单位向量
float3 TexelDir(uint3 id,uint size){
    float2 uv = (id.xy + 0.5f) / size * 2.0f - 1.0f;
    return normalize(CubeDir(id.z,uv));
}

// 以 n 为 z 轴搭一个坐标系，把"切线空间"里的方向 d 转到世界：和法线贴图的 TBN 一样
float3 TangentToWorld(float3 d,float3 n){
    float3 up = abs(n.y) < 0.9999f ? float3(0.0f,1.0f,0.0f) : float3(1.0f,0.0f,0.0f);
    float3 t = normalize(cross(n,up));
    float3 b = normalize(cross(t,n));
    return mul(d,float3x3(t,b,n));
}

// ======================== ① 辐照度图 ========================
// E(n) = ∫半球 L(ω) cosθ dω，dω = sinθ dθ dφ。存 E / π：用的时候乘上漫反射颜色就是漫反射
[numthreads(8,8,1)]
void CS_Irradiance(uint3 id : SV_DispatchThreadID){
    uint size,height,faces;
    gOutCube.GetDimensions(size,height,faces);
    if(id.x >= size || id.y >=size){
        return;
    }
    float3 n = TexelDir(id,size);
    
    float3 sum = 0.0f;
    for(uint i = 0;i<kIrrPhiSteps;i++){
        float phi = (i+0.5f)*(PI*2 / kIrrPhiSteps);   // 一圈分 kIrrPhiSteps（128）份：分母是这一层循环自己的份数
        for(uint j =0;j<kIrrThetaSteps;j++){
            float theta = (j + 0.5f) * (0.5f * PI / kIrrThetaSteps);   // 0～90°，分 32 份
            float3 d = float3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta)); // 切线空间：z 轴 = 法线
            float3 dir = TangentToWorld(d, n);
            sum += gEnvMap.SampleLevel(gsamLinearWrap, dir, kIrrSrcMip).rgb * sin(theta)*cos(theta);
        }
    }
    gOutCube[id] = float4(PI * sum / (kIrrPhiSteps * kIrrThetaSteps), 1.0f);
}

// 按 GGX 的形状挑一个半程向量 h（切线空间，z 轴 = 法线）。xi 是 0～1 的两个数，a = α（已经平方过的粗糙度）
// cosθ 的公式：把 D_GGX 当成概率分布、求它的反函数得到（AI 给的）。越光滑，挑出来的 h 越贴着法线
float3 ImportanceSampleGGX(float2 xi, float a){
    float phi = PI*2*xi.x;
    float cosTheta = sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
    float sinTheta = sqrt(1-cosTheta*cosTheta);
    return float3(sinTheta*cos(phi),sinTheta*sin(phi),cosTheta);
}

// ======================== ② 预滤波环境贴图 ========================
// 假设视线 v = 法线 n = 反射方向 r，按 GGX 挑 1024 个 h，把 v 关于 h 镜像得到光线 l，按 NoL 加权平均
[numthreads(8, 8, 1)]
void CS_Prefilter(uint3 id : SV_DispatchThreadID)
{
    uint size, height, faces;
    gOutCube.GetDimensions(size, height, faces); // 当前这一级 mip 的大小（UAV 只看得见这一级）
    if (id.x >= size || id.y >= size)
    return;
    float3 n = TexelDir(id, size);
    float3 v = n;
    float r = max(MinRoughness, gRoughness);
    float a = r*r;
    uint srcSize, srcHeight, srcMips;
    gEnvMap.GetDimensions(0, srcSize, srcHeight, srcMips);
    float saTexel = 4.0f * PI / (6.0f * srcSize * srcSize); // 天空图第 0 级一个像素占的立体角
    float3 sum = 0.0f;
    float weight = 0.0f;
    for (uint i = 0; i < kPrefilterSamples; i++)
    {
        float3 h = TangentToWorld(ImportanceSampleGGX(Hammersley(i, kPrefilterSamples), a), n);
        float3 l = -reflect(v,h); // v 关于 h 的镜像
        float NoL = dot(n, l);
        if (NoL > 0.0f)
        {
            // 样本稀的地方读糊一点的 mip（AI 给的）：一个样本"管"的立体角 = 1 / (N·pdf)，pdf = D·NoH / (4·VoH) = D / 4（v = n）
            float pdf = D_GGX(saturate(dot(n, h)), a) / 4.0f;
            float saSample = 1.0f / (kPrefilterSamples * pdf + 1e-6f);
            float mip = max(0.5f * log2(saSample / saTexel) + 1.0f, 0.0f);
            sum += gEnvMap.SampleLevel(gsamLinearWrap, l, mip).rgb * NoL;
            weight += NoL;
        }
    }
    gOutCube[id] = float4(sum / weight, 1.0f);
}

// ======================== ③ BRDF LUT ========================
// x = NoV，y = 粗糙度。镜面积分 ≈ 预滤波 × (F0·A + B)，这里算 (A, B)。法线固定为 (0, 0, 1)，v 放在 xz 平面里
[numthreads(8, 8, 1)]
void CS_BrdfLut(uint3 id : SV_DispatchThreadID){
    uint width, height;
    gOutLut.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
        return;
    float NoV = (id.x+0.5f)/width;                       // 横轴：这一列的像素中心 → 0～1
    float r = max(MinRoughness, (id.y+0.5f)/height);      // 竖轴：这一行的像素中心 → 0～1
    float a = r*r;
    float3 v =float3(sqrt(1-NoV*NoV),0,NoV);
    float2 sum = 0.0f;
    for(uint i =0;i<kLutSamples;i++){
        float3 h = ImportanceSampleGGX(Hammersley(i, kLutSamples), a); // 法线就是 z 轴，不用转
        float3 l =-reflect(v,h);
        float NoL =saturate(l.z);
        float NoH = saturate(h.z);
        float VoH = saturate(dot(v,h));
        if(NoL >0.0f){
            float vis = V_SmithGGXCorrelated(NoV, NoL, a) * NoL * VoH / NoH;    // f·NoL / pdf：D 约掉了。4 留到最后乘
            float fc = pow((1-VoH),5);                // Schlick 里的 (1 − VoH)⁵
            sum += float2(vis * (1 - fc), vis * fc);      // (A 的这一份, B 的这一份)
        }
    }
    gOutLut[id.xy] = sum * 4 / kLutSamples;
}