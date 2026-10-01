// 公告牌树：VS 原样传点，GS 把一个点扩成正对相机的四边形，PS 按 PrimID 从纹理数组里挑一张树
// 根签名和 Triangle.hlsl 共用：b1 = Pass 常量，b2 = 材质，t0 = 纹理（这里是纹理数组），s0 = 采样器
#include "LightingUtil.hlsl"

ConstantBuffer<PassConstants> gPass : register(b1);
ConstantBuffer<MaterialConstants> gMat : register(b2);

Texture2DArray gTreeMapArray : register(t0);
SamplerState gsamLinearWrap : register(s0);

struct VertexIn
{
    float3 PosW : POSITION;
    float2 SizeW : SIZE;
};

struct VertexOut
{
    float3 CenterW : CENTER;
    float2 SizeW : SIZE;
};

struct GeoOut
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD;
    uint PrimID : SV_PrimitiveID;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    vout.CenterW = vin.PosW; // 不做任何变换：GS 要在世界空间里搭四边形
    vout.SizeW = vin.SizeW;
    return vout;
}

float4 PS(GeoOut pin) : SV_Target
{
    float4 res = float4(0, 0, 0, 0);
    float3 uvw = float3(pin.TexC, pin.PrimID % 3); // 第 3 个坐标 = 第几张图（我们只有 3 张）
    float4 TexColor = gTreeMapArray.Sample(gsamLinearWrap, uvw);

    // 光照用的材质：反照率要带上贴图颜色，其余原样照抄 gMat
    MaterialConstants mat = gMat;
    mat.DiffuseAlbedo = TexColor * gMat.DiffuseAlbedo;
#ifdef ALPHA_TEST
    clip(mat.DiffuseAlbedo.a - 0.1f);
#endif

    float3 ambient = gPass.AmbientLight.rgb * mat.DiffuseAlbedo.rgb;
    float3 toEye = gPass.EyePosW - pin.PosW;
    float distToEye = length(toEye);
    toEye /= distToEye;
    float3 normal = normalize(pin.NormalW); // 插值之后长度不一定是 1
    float3 direct = ComputeLighting(gPass.Lights, mat, pin.PosW, normal, toEye);
    res = float4(ambient + direct, mat.DiffuseAlbedo.a);
#ifdef FOG
    res.rgb = ApplyFog(res.rgb, distToEye, gPass.FogColor, gPass.FogStart, gPass.FogRange);
#endif
    return res;
}

[maxvertexcount(4)]
void GS(
    point VertexOut gin[1], // 输入：1 个点
    uint primID :SV_PrimitiveID, // 这次 draw 里的第几个点，也就是第几棵树
    inout TriangleStream<GeoOut> triStream) // 输出：三角形带（书上 p407 漏了这个名字）
{
    //公告牌自己的三根轴
    float3 up = float3(0.f, 1.f, 0.f);
    float3 look = gPass.EyePosW - gin[0].CenterW;
    look.y = 0;
    look = normalize(look);
    float3 right = cross(up, look);

    //四个角（世界空间）
    float halfWidth = 0.5f * gin[0].SizeW.x;
    float halfHeight = 0.5f * gin[0].SizeW.y;
    float4 v[4];
    v[0] = float4(gin[0].CenterW + halfWidth * right - halfHeight * up, 1.0f); // 屏幕左下
    v[1] = float4(gin[0].CenterW + halfWidth * right + halfHeight * up, 1.0f); // 空 4：屏幕左上
    v[2] = float4(gin[0].CenterW - halfWidth * right - halfHeight * up, 1.0f); // 空 5：屏幕右下
    v[3] = float4(gin[0].CenterW - halfWidth * right + halfHeight * up, 1.0f); // 空 6：屏幕右上

    //四个角的纹理坐标，顺序和 v 一一对应
    float2 texC[4] = {
        float2(0.0f, 1.0f), // 左下
        float2(0.0f, 0.0f), // 左上
        float2(1.0f, 1.0f), // 右下
        float2(1.0f, 0.0f) // 右上
    };
    
    //一个一个输出：变到裁剪空间，填满 GeoOut 的 5 个字段，Append
    GeoOut gout;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        gout.PosH = mul(v[i],gPass.ViewProj);     // 空 7：世界空间 → 裁剪空间
        gout.PosW = v[i].xyz;
        gout.NormalW = look;      // 法线朝向眼睛（所以逆光的时候是暗的）
        gout.TexC = texC[i];
        gout.PrimID = primID;
        triStream.Append(gout);
    }

}
