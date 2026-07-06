cbuffer TranslucentPreLightingMeshGuideCB : register(b0)
{
    float4x4 WorldViewMatrix;
    float4x4 WorldViewProjectionMatrix;
    float4x4 NormalWorldViewMatrix;
    float4 BaseColorFactor;
    float4 RenderTargetParams;
    float4 EffectParams;
    uint4 StochasticParams;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL0;
    float2 uv       : TEXCOORD0;
    float3 tangent  : TANGENT0;
};

struct PSInput
{
    float4 position   : SV_POSITION;
    float3 viewPos    : TEXCOORD0;
    float3 viewNormal : TEXCOORD1;
};


PSInput VSMain(VSInput input)
{
    PSInput result;
    result.position = mul(float4(input.position, 1.0f), WorldViewProjectionMatrix);
    result.viewPos = mul(float4(input.position, 1.0f), WorldViewMatrix).xyz;
    result.viewNormal = normalize(mul(float4(input.normal, 0.0f), NormalWorldViewMatrix).xyz);
    return result;
}

float4 PSMain(PSInput input) : SV_Target0
{
    float alpha = saturate(BaseColorFactor.a);
    float2 invTargetSize = RenderTargetParams.xy;
    float2 screenUv = saturate(input.position.xy * invTargetSize);
    float3 viewNormal = normalize(input.viewNormal);
    float3 viewDir = normalize(-input.viewPos);
    if (dot(viewNormal, viewDir) < 0.0f)
        viewNormal = -viewNormal;

    float noV = saturate(dot(viewNormal, viewDir));
    float grazing = 1.0f - noV;
    float2 normalOffset = float2(viewNormal.x, -viewNormal.y);

    float refractionPixels = max(EffectParams.x, 0.0f);
    float2 refractionDeltaUv = normalOffset * refractionPixels * invTargetSize * (0.35f + 0.65f * grazing);

    // PSO uses AdditiveAlpha blending: RGB is multiplied by source alpha and
    // alpha is additively accumulated. Store a composable offset rather than
    // an absolute UV so overlapping translucent meshes do not overwrite each
    // other like opaque layers.
    return float4(refractionDeltaUv, 1.0f, alpha);
}
