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

float4 PSFront(PSInput input) : SV_Target0
{
    float frontDepth = max(-input.viewPos.z, 0.0f);
    return float4(max(BaseColorFactor.rgb, 0.0f.xxx), frontDepth);
}
