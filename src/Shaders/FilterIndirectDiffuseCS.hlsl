Texture2D InputTex : register(t0);
Texture2D DepthTex : register(t1);
Texture2D GeoNormalTex : register(t2);

RWTexture2D<float4> OutputTex : register(u0);


cbuffer FilterIndirectDiffuseConstant : register(b0)
{
    uint Iteration;
};



[numthreads(32, 32, 1)]
void FilterIndirectDiffuse( uint3 DTid : SV_DispatchThreadID )
{
	int StepSize =  int(1u << (Iteration - 1));

	float2 CenterPos = DTid.xy;
	const int r = 1;
	float3 SumColor = float3(0, 0, 0);
	for(int yy = -r; yy <= r; yy++) 
	{
		for(int xx = -r; xx <= r; xx++) 
		{
			float2 SamplePos = CenterPos + float2(xx, yy) * StepSize;
			float3 Sample = InputTex[SamplePos];

			SumColor += Sample;
		}
	}
    OutputTex[DTid.xy] = float4(SumColor/9.0f, 1);
}