#include "Common.hlsl"

RWTexture2D<float4> FroxelAtlas : register(u0);
Texture2D<float> DepthTex : register(t0);
Texture2D<float4> FroxelAtlasRead : register(t1);

cbuffer FogCB : register(b0)
{
    float4x4 InvViewMatrix;
    float4x4 InvProjMatrix;
    float4 FogColorAndDensity;   // rgb = albedo/scatter color, a = density
    float4 HeightParams;         // x = height, y = falloff, z = start distance, w = max distance
    float4 LightingParams;       // x = anisotropy, y = ambient strength, z = directional strength, w = max opacity
    float4 GridParams;           // x = grid X, y = grid Y, z = grid Z, w = grid pixel size
    float4 LightDirAndIntensity; // xyz = direction to sun, w = intensity
    float4 FrameParams;          // x = frame index
    float2 RTSize;
    float2 Padding;
};

float3 CameraWorldPosition()
{
    return mul(float4(0.0f, 0.0f, 0.0f, 1.0f), InvViewMatrix).xyz;
}

float3 ReconstructViewRay(float2 uv)
{
    float2 screenPosition = uv * 2.0f - 1.0f;
    screenPosition.y = -screenPosition.y;

    float4 viewFarH = mul(float4(screenPosition, 1.0f, 1.0f), InvProjMatrix);
    float3 viewFar = viewFarH.xyz / max(abs(viewFarH.w), 1.0e-6f);
    return normalize(viewFar);
}

float3 ReconstructWorldRay(float2 uv)
{
    float3 viewRay = ReconstructViewRay(uv);
    return normalize(mul(float4(viewRay, 0.0f), InvViewMatrix).xyz);
}

float ReconstructViewDistance(uint2 pixel, float deviceDepth)
{
    if (deviceDepth >= 0.999999f)
        return HeightParams.w;

    float2 uv = (float2(pixel) + 0.5f) / max(RTSize, float2(1.0f, 1.0f));
    float2 screenPosition = uv * 2.0f - 1.0f;
    screenPosition.y = -screenPosition.y;

    float3 viewPosition = GetViewPosition(deviceDepth, screenPosition, InvProjMatrix);
    return length(viewPosition);
}

float SliceT(uint z)
{
    float gridZ = max(GridParams.z, 1.0f);
    return (float(z) + 0.5f) / gridZ;
}

float SliceBoundaryT(uint z)
{
    float gridZ = max(GridParams.z, 1.0f);
    return float(z) / gridZ;
}

float SliceDistanceFromT(float t)
{
    float startDistance = max(HeightParams.z, 0.0f);
    float maxDistance = max(HeightParams.w, startDistance + 1.0f);
    float curvedT = t * t;
    return lerp(startDistance, maxDistance, curvedT);
}

float SliceCenterDistance(uint z)
{
    return SliceDistanceFromT(SliceT(z));
}

float SliceStepLength(uint z)
{
    float d0 = SliceDistanceFromT(SliceBoundaryT(z));
    float d1 = SliceDistanceFromT(SliceBoundaryT(z + 1));
    return max(d1 - d0, 0.0f);
}

float PhaseHenyeyGreenstein(float cosTheta, float g)
{
    g = clamp(g, -0.9f, 0.9f);
    float g2 = g * g;
    float denom = max(1.0f + g2 - 2.0f * g * cosTheta, 1.0e-4f);
    return (1.0f - g2) / (4.0f * 3.14159265f * denom * sqrt(denom));
}

float HeightDensity(float worldY)
{
    float height = HeightParams.x;
    float falloff = max(HeightParams.y, 0.0f);
    return exp(-max(worldY - height, 0.0f) * falloff);
}

[numthreads(8, 8, 1)]
void VolumetricFogBuildCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint gridX = (uint)GridParams.x;
    uint gridY = (uint)GridParams.y;
    uint gridZ = (uint)GridParams.z;
    uint3 froxel = dispatchThreadId;
    if (froxel.x >= gridX || froxel.y >= gridY || froxel.z >= gridZ)
        return;

    float2 uv = (float2(froxel.xy) + 0.5f) / max(float2(gridX, gridY), float2(1.0f, 1.0f));
    float3 worldRay = ReconstructWorldRay(uv);
    float3 cameraPosition = CameraWorldPosition();
    float viewDistance = SliceCenterDistance(froxel.z);
    float3 worldPosition = cameraPosition + worldRay * viewDistance;

    float density = max(FogColorAndDensity.a, 0.0f) * HeightDensity(worldPosition.y);
    float alpha = saturate(1.0f - exp(-density * SliceStepLength(froxel.z)));
    alpha *= saturate(LightingParams.w);

    float3 lightDir = normalize(LightDirAndIntensity.xyz + float3(0.0f, 1.0e-6f, 0.0f));
    float cosTheta = dot(lightDir, -worldRay);
    float phase = PhaseHenyeyGreenstein(cosTheta, LightingParams.x);
    float directional = max(LightingParams.z, 0.0f) * max(LightDirAndIntensity.w, 0.0f) * phase * 4.0f;
    float ambient = max(LightingParams.y, 0.0f);
    float lighting = ambient + directional;

    float3 scatter = max(FogColorAndDensity.rgb, 0.0f.xxx) * lighting * alpha;
    FroxelAtlas[uint2(froxel.x, froxel.y + froxel.z * gridY)] = float4(scatter, alpha);
}

float4 SampleFroxelAtlas(float2 froxelUv, uint z)
{
    float gridX = max(GridParams.x, 1.0f);
    float gridY = max(GridParams.y, 1.0f);
    uint gridYInt = (uint)gridY;
    uint gridXInt = (uint)gridX;

    float2 atlasCoord = froxelUv * float2(gridX, gridY) - 0.5f;
    int2 baseCoord = int2(floor(atlasCoord));
    float2 fracCoord = frac(atlasCoord);

    int x0 = clamp(baseCoord.x, 0, int(gridXInt) - 1);
    int y0 = clamp(baseCoord.y, 0, int(gridYInt) - 1);
    int x1 = clamp(baseCoord.x + 1, 0, int(gridXInt) - 1);
    int y1 = clamp(baseCoord.y + 1, 0, int(gridYInt) - 1);
    int rowOffset = int(z * gridYInt);

    float4 s00 = FroxelAtlasRead.Load(int3(x0, y0 + rowOffset, 0));
    float4 s10 = FroxelAtlasRead.Load(int3(x1, y0 + rowOffset, 0));
    float4 s01 = FroxelAtlasRead.Load(int3(x0, y1 + rowOffset, 0));
    float4 s11 = FroxelAtlasRead.Load(int3(x1, y1 + rowOffset, 0));

    float4 sx0 = lerp(s00, s10, fracCoord.x);
    float4 sx1 = lerp(s01, s11, fracCoord.x);
    return lerp(sx0, sx1, fracCoord.y);
}

[numthreads(8, 8, 1)]
void VolumetricFogCompositeCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= (uint)RTSize.x || pixel.y >= (uint)RTSize.y)
        return;

    float2 uv = (float2(pixel) + 0.5f) / max(RTSize, float2(1.0f, 1.0f));
    float viewDistance = ReconstructViewDistance(pixel, DepthTex.Load(int3(pixel, 0)).x);
    float maxDistance = max(HeightParams.w, HeightParams.z + 1.0f);
    float integrationDistance = min(viewDistance, maxDistance);

    uint gridZ = (uint)max(GridParams.z, 1.0f);
    float3 accumColor = 0.0f.xxx;
    float accumAlpha = 0.0f;

    [loop]
    for (uint z = 0; z < gridZ; ++z)
    {
        float sliceDistance = SliceCenterDistance(z);
        if (sliceDistance > integrationDistance)
            break;

        float4 sampleValue = SampleFroxelAtlas(uv, z);
        float alpha = saturate(sampleValue.a);
        accumColor += (1.0f - accumAlpha) * sampleValue.rgb;
        accumAlpha += (1.0f - accumAlpha) * alpha;
        if (accumAlpha >= 0.995f)
            break;
    }

    float4 src = FroxelAtlas[pixel];
    src.rgb = src.rgb * (1.0f - accumAlpha) + accumColor;
    FroxelAtlas[pixel] = src;
}
