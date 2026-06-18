cbuffer ClearParams : register(b0)
{
    float4 ClearValue;
    uint2 Extent;
    uint2 _Pad;
};

RWTexture2D<float2> OutTex2 : register(u0);

[numthreads(8, 8, 1)]
void ClearTextureFloat2(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Extent.x || id.y >= Extent.y)
        return;
    OutTex2[id.xy] = ClearValue.xy;
}
