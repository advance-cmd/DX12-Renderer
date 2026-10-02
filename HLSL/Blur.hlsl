// 可分离高斯模糊：横向一遍 + 纵向一遍，模糊半径最大 5（11 个权重）
// 用模糊专用的根签名：参数 0 = 12 个根常量（b0），参数 1 = 输入 SRV（t0），参数 2 = 输出 UAV（u0）
struct cbSettings
{
    int gBlurRadius;
    float w0;
    float w1;
    float w2;
    float w3;
    float w4;
    float w5; // 11 个权重一个个列出来：
    float w6;
    float w7;
    float w8;
    float w9;
    float w10; // 根常量是紧挨着排的，cbuffer 里的数组每个元素占 16 字节，对不上
};

ConstantBuffer<cbSettings> BlurSettings : register(b0);

static const int gMaxBlurRadius = 5;

Texture2D gInput : register(t0); //输入：只读
RWTexture2D<float4> gOutput : register(u0); //输出：可读可写

#define N 256
#define CacheSize (N+2*gMaxBlurRadius)
groupshared float4 gCache[CacheSize]; //工作台：256 + 10 = 266 格

[numthreads(N,1,1)] //一个线程组 = 一行里的256个像素
void HorzBlurCS(int3 groupThreadID :SV_GroupThreadID, int3 dispatchThreadID :SV_DispatchThreadID)
{
    float weights[11] = {
        BlurSettings.w0,
        BlurSettings.w1,
        BlurSettings.w2,
        BlurSettings.w3,
        BlurSettings.w4,
        BlurSettings.w5,
        BlurSettings.w6,
        BlurSettings.w7,
        BlurSettings.w8,
        BlurSettings.w9,
        BlurSettings.w10,
    };

    // 最左边 R 个线程多搬一个：本组左边界外面的像素，放进 gCache[0 .. R-1]
    if (groupThreadID.x < BlurSettings.gBlurRadius)
    {
        int x = max(dispatchThreadID.x - BlurSettings.gBlurRadius, 0); // 空 1
        gCache[groupThreadID.x] = gInput[int2(x, dispatchThreadID.y)];
    }
    // 最右边 R 个线程多搬一个：本组右边界外面的像素，放进 gCache[N+R .. N+2R-1]
    if (groupThreadID.x >= N - BlurSettings.gBlurRadius)
    {
        int x = min(dispatchThreadID.x + BlurSettings.gBlurRadius,gInput.Length.x - 1); // 空 2
        gCache[groupThreadID.x + BlurSettings.gBlurRadius*2] = gInput[int2(x, dispatchThreadID.y)]; // 空 3
    }
    // 每个线程搬自己的像素。最右边那个组多出来的线程，下标被夹在图里面
    gCache[groupThreadID.x + BlurSettings.gBlurRadius] = gInput[min(dispatchThreadID.xy, gInput.Length.xy - 1)];
    
    GroupMemoryBarrierWithGroupSync();
    
    float4 blurColor = float4(0, 0, 0, 0);
    for (int i = -BlurSettings.gBlurRadius; i <= BlurSettings.gBlurRadius; ++i)
    {
        int k = groupThreadID.x + BlurSettings.gBlurRadius + i;                                     // 空 5
        blurColor += weights[i + BlurSettings.gBlurRadius] * gCache[k];
    }
    gOutput[dispatchThreadID.xy] = blurColor;
}

[numthreads(1,N,1)] //一个线程组 = 一行里的256个像素
void VertBlurCS(int3 groupThreadID :SV_GroupThreadID, int3 dispatchThreadID :SV_DispatchThreadID)
{
    float weights[11] = {
        BlurSettings.w0,
        BlurSettings.w1,
        BlurSettings.w2,
        BlurSettings.w3,
        BlurSettings.w4,
        BlurSettings.w5,
        BlurSettings.w6,
        BlurSettings.w7,
        BlurSettings.w8,
        BlurSettings.w9,
        BlurSettings.w10,
    };

    // 最左边 R 个线程多搬一个：本组左边界外面的像素，放进 gCache[0 .. R-1]
    if (groupThreadID.y < BlurSettings.gBlurRadius)
    {
        int y = max(dispatchThreadID.y - BlurSettings.gBlurRadius, 0); // 空 1
        gCache[groupThreadID.y] = gInput[int2(dispatchThreadID.x, y)];
    }
    // 最右边 R 个线程多搬一个：本组右边界外面的像素，放进 gCache[N+R .. N+2R-1]
    if (groupThreadID.y >= N - BlurSettings.gBlurRadius)
    {
        int y = min(dispatchThreadID.y + BlurSettings.gBlurRadius,gInput.Length.y - 1); // 空 2
        gCache[groupThreadID.y + BlurSettings.gBlurRadius*2] = gInput[int2(dispatchThreadID.x, y)]; // 空 3
    }
    // 每个线程搬自己的像素。最右边那个组多出来的线程，下标被夹在图里面
    gCache[groupThreadID.y + BlurSettings.gBlurRadius] = gInput[min(dispatchThreadID.xy, gInput.Length.xy - 1)];
    
    GroupMemoryBarrierWithGroupSync();
    
    float4 blurColor = float4(0, 0, 0, 0);
    for (int i = -BlurSettings.gBlurRadius; i <= BlurSettings.gBlurRadius; ++i)
    {
        int k = groupThreadID.y + BlurSettings.gBlurRadius + i;                                     // 空 5
        blurColor += weights[i + BlurSettings.gBlurRadius] * gCache[k];
    }
    gOutput[dispatchThreadID.xy] = blurColor;
}
