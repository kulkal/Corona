cbuffer ClearParams : register(b0)
{
    float4 ClearValue;
    uint2 Extent;
    uint2 _Pad;
};

RWTexture2D<float> OutTex1 : register(u0);

[numthreads(8, 8, 1)]
void ClearTextureFloat1(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Extent.x || id.y >= Extent.y)
        return;
    OutTex1[id.xy] = ClearValue.x;
}
