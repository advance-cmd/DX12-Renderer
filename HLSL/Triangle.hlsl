
struct ObjectConstants
{
    float4x4 MVP;
};
ConstantBuffer<ObjectConstants> gObjConstants :register(b0);

Texture2D gDiffuseMap : register(t0);
SamplerState gsamLinearWrap : register(s0);

struct VertexIn
{
    float3 PosL : POSITION; //位置(L=Local，局部空间)
    float2 TexC : TEXCOORD;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;  //H = homogeneous，齐次裁剪空间
    float2 TexC : TEXCOORD;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vOut;
    vOut.PosH = mul(float4(vin.PosL, 1.0f), gObjConstants.MVP);
    vOut.TexC = vin.TexC;
    return vOut;
}

float4 PS(VertexOut pin) : SV_Target{
    return gDiffuseMap.Sample(gsamLinearWrap, pin.TexC);
}
