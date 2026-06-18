cbuffer C : register(b0) { uint mul; };
RWStructuredBuffer<uint> O : register(u0);
[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) { O[id.x] = mul * 7u; }
