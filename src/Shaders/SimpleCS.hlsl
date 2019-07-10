//Texture2D input : register(t0);
RWTexture2D<float4> output : register(u0);

[numthreads(32, 32, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    output[DTid.xy] = float4(0, 0, 1, 0);
}