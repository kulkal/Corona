
Texture2D<uint> LumaTex : register( t0 );
RWByteAddressBuffer Histogram : register( u0 );

groupshared uint g_TileHistogram[256];

[numthreads( 16, 16, 1 )]
void GenerateHistogram( uint GI : SV_GroupIndex, uint3 DTid : SV_DispatchThreadID )
{
    g_TileHistogram[GI] = 0;

    GroupMemoryBarrierWithGroupSync();

    // Loop 24 times until the entire column has been processed
    for (uint TopY = 0; TopY < 384; TopY += 16)
    {
        uint QuantizedLogLuma = LumaTex[DTid.xy + uint2(0, TopY)];
        InterlockedAdd( g_TileHistogram[QuantizedLogLuma], 1 );
    }

    GroupMemoryBarrierWithGroupSync();

    Histogram.InterlockedAdd( GI * 4, g_TileHistogram[GI] );
}



[numthreads( 256, 1, 1 )]
void ClearHistogram( uint GI : SV_GroupIndex, uint3 DTid : SV_DispatchThreadID )
{
   Histogram.Store(DTid.x * 4, 0);
}
