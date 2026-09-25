struct VertexIn
{
    float3 PosL : POSITION; //位置(L=Local，局部空间)
    float4 Color : COLOR;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;  //H = homogeneous，齐次裁剪空间
    float4 Color : COLOR;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vOut;
    vOut.PosH = float4(vin.PosL * 0.5f + float3(0.0f, 0.0f, 0.5f), 1.0f);
    vOut.Color = vin.Color;
    return vOut;
}

float4 PS(VertexOut pin) : SV_Target{
    return pin.Color;
}
