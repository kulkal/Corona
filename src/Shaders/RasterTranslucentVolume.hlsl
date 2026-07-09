cbuffer RasterTranslucentVolumeCB : register(b0)
{
    float4x4 WorldViewMatrix;
    float4x4 WorldViewProjectionMatrix;
    float4 BaseColorFactor;
    // x density, y scattering albedo/strength, z anisotropy
    float4 VolumeParams;
    // xy inverse low-resolution size, zw low-resolution size
    float4 TargetParams;
    float4 ViewLightDirAndIntensity;
    float4 LightColorAndSkyIntensity;
    float4 SkyColorTop;
    float4 SkyColorBottom;
};

Texture2D<float4> BackDepthTex : register(t0);
Texture2D<float4> ShadowTex : register(t1);

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL0;
    float2 uv : TEXCOORD0;
    float3 tangent : TANGENT0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 viewPos : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput result;
    result.position = mul(float4(input.position, 1.0f), WorldViewProjectionMatrix);
    result.viewPos = mul(float4(input.position, 1.0f), WorldViewMatrix).xyz;
    return result;
}

float4 PSBack(PSInput input, bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
    if (isFrontFace)
        discard;
    return float4(max(-input.viewPos.z, 0.0f), 0.0f, 0.0f, 1.0f);
}

float4 PSFront(PSInput input, bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
    if (!isFrontFace)
        discard;

    uint2 pixel = uint2(max(input.position.xy, 0.0f.xx));
    float frontDepth = max(-input.viewPos.z, 0.0f);
    float backDepth = BackDepthTex.Load(int3(pixel, 0)).x;
    if (backDepth <= frontDepth + 1.0e-4f)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    float3 viewRayDir = normalize(input.viewPos);
    float segmentLength = (backDepth - frontDepth) / max(abs(viewRayDir.z), 0.05f);
    float density = max(VolumeParams.x, 0.0f);
    float segmentT = exp(-density * segmentLength);

    float2 uv = (float2(pixel) + 0.5f) * TargetParams.xy;
    uint shadowWidth;
    uint shadowHeight;
    ShadowTex.GetDimensions(shadowWidth, shadowHeight);
    uint2 shadowPixel = min(
        uint2(uv * float2(shadowWidth, shadowHeight)),
        uint2(max(shadowWidth, 1u) - 1u, max(shadowHeight, 1u) - 1u));
    float visibility = saturate(ShadowTex.Load(int3(shadowPixel, 0)).x);

    float3 lightDir = normalize(ViewLightDirAndIntensity.xyz);
    float anisotropy = clamp(VolumeParams.z, -0.9f, 0.9f);
    float cosTheta = dot(-viewRayDir, lightDir);
    float phaseDenom = max(1.0f + anisotropy * anisotropy - 2.0f * anisotropy * cosTheta, 1.0e-3f);
    float phase = (1.0f - anisotropy * anisotropy) /
        (4.0f * 3.14159265359f * pow(phaseDenom, 1.5f));

    float3 direct = max(LightColorAndSkyIntensity.rgb, 0.0f.xxx) *
        max(ViewLightDirAndIntensity.w, 0.0f) * visibility * phase;
    float3 ambient = max(lerp(SkyColorBottom.rgb, SkyColorTop.rgb, 0.5f), 0.0f.xxx) *
        max(LightColorAndSkyIntensity.w, 0.0f);
    float3 scattering = max(BaseColorFactor.rgb, 0.0f.xxx) *
        max(VolumeParams.y, 0.0f) * (1.0f - segmentT) * (direct + ambient);
    return float4(max(scattering, 0.0f.xxx), saturate(segmentT));
}
