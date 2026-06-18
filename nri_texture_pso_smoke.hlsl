RWTexture2D<float4> OutTex : register(u0);
[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) { OutTex[id.xy] = float4(0.25f, 0.5f, 0.75f, 1.0f); }
