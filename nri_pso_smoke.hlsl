RWStructuredBuffer<uint> OutBuf : register(u0);
[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) { OutBuf[id.x] = 1u; }
