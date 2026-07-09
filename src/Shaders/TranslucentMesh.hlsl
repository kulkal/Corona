#if defined(VULKAN_SPIRV)
#undef TRANSLUCENT_ENABLE_RT_REFLECTION
#define TRANSLUCENT_ENABLE_RT_REFLECTION 0
#endif

#ifndef TRANSLUCENT_ENABLE_RT_REFLECTION
#define TRANSLUCENT_ENABLE_RT_REFLECTION 0
#endif

#ifndef TRANSLUCENT_SURFACE_ONLY
#define TRANSLUCENT_SURFACE_ONLY 0
#endif

#if TRANSLUCENT_ENABLE_RT_REFLECTION
#include "Common.hlsl"
#include "BindlessResources.hlsli"
#endif

cbuffer TranslucentMeshCB : register(b0)
{
    float4x4 WorldViewMatrix;
    float4x4 WorldViewProjectionMatrix;
    float4x4 WorldMatrix;
    float4x4 ViewProjectionMatrix;
    float4x4 NormalWorldViewMatrix;
    float4x4 NormalWorldMatrix;
    float4 BaseColorFactor;
    float4 EffectParams;
    float4 RenderTargetParams;
    float4 CameraPositionAndRayParams;
    float4 SurfaceDepthParams;
    float4 ReflectionHitLightDirAndIntensity;
    float4 ReflectionHitLightColorAndViewSpread;
    float4 GlassParams;
    uint4 StochasticParams;
};

Texture2D SceneColorTex : register(t0);
Texture2D RefractedColorTex : register(t3);
#if TRANSLUCENT_ENABLE_RT_REFLECTION
RaytracingAccelerationStructure gRtScene : register(t1);
#endif
#if !TRANSLUCENT_SURFACE_ONLY
Texture2D PrevCompositeGuideTex : register(t2);
#endif
SamplerState samplerClamp : register(s0);

static const uint TRANSLUCENT_FLAG_SURFACE_PASS = 1u << 0;
static const uint TRANSLUCENT_FLAG_REFLECTION_HIT_LIGHTING_FALLBACK = 1u << 1;
static const uint TRANSLUCENT_FLAG_RT_REFRACTION_COLOR = 1u << 2;
static const uint TRANSLUCENT_RT_MASK_OPAQUE = 0x01u;

bool TranslucentFlagEnabled(uint flag)
{
    return (StochasticParams.x & flag) != 0u;
}

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
    float3 worldPos   : TEXCOORD2;
    float3 worldNormal : TEXCOORD3;
};

uint HashUint(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float Random01(uint2 pixel, uint frameIndex, uint drawSeed)
{
    uint h = pixel.x * 0x8da6b343u;
    h ^= pixel.y * 0xd8163841u;
    h ^= frameIndex * 0xcb1ab31fu;
    h ^= drawSeed * 0x165667b1u;
    return (HashUint(h) & 0x00ffffffu) * (1.0f / 16777216.0f);
}

float2 SampleConcentricDisk(float2 randomSample)
{
    float2 offset = randomSample * 2.0f - 1.0f;
    if (abs(offset.x) < 1.0e-6f && abs(offset.y) < 1.0e-6f)
        return float2(0.0f, 0.0f);

    float radius;
    float theta;
    if (abs(offset.x) > abs(offset.y))
    {
        radius = offset.x;
        theta = 0.78539816339f * (offset.y / offset.x);
    }
    else
    {
        radius = offset.y;
        theta = 1.57079632679f - 0.78539816339f * (offset.x / offset.y);
    }
    return radius * float2(cos(theta), sin(theta));
}

float3 SampleRoughReflectionDirection(
    float3 incident,
    float3 normal,
    float roughness,
    uint2 pixel,
    uint frameIndex,
    uint drawSeed)
{
    float3 perfectReflection = normalize(reflect(incident, normal));
    roughness = saturate(roughness);
    if (roughness <= 1.0e-4f)
        return perfectReflection;

    float2 randomSample = float2(
        Random01(pixel, frameIndex, drawSeed),
        Random01(pixel, frameIndex, drawSeed ^ 0x9e3779b9u));
    float alpha = max(roughness * roughness, 1.0e-3f);
    float alphaSquared = alpha * alpha;
    float phi = 6.28318530718f * randomSample.x;
    float cosTheta = sqrt(saturate(
        (1.0f - randomSample.y) /
        max(1.0f + (alphaSquared - 1.0f) * randomSample.y, 1.0e-5f)));
    float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));

    float3 tangent = abs(normal.z) < 0.999f
        ? normalize(cross(float3(0.0f, 0.0f, 1.0f), normal))
        : normalize(cross(float3(0.0f, 1.0f, 0.0f), normal));
    float3 bitangent = cross(normal, tangent);
    float3 microfacetNormal = normalize(
        tangent * (cos(phi) * sinTheta) +
        bitangent * (sin(phi) * sinTheta) +
        normal * cosTheta);
    float3 sampledReflection = normalize(reflect(incident, microfacetNormal));
    return dot(sampledReflection, normal) > 1.0e-4f
        ? sampledReflection
        : perfectReflection;
}

uint2 ClampGuidePixel(int2 pixel, uint2 dims)
{
    return uint2(
        clamp(pixel.x, 0, int(max(dims.x, 1u) - 1u)),
        clamp(pixel.y, 0, int(max(dims.y, 1u) - 1u)));
}

#if !TRANSLUCENT_SURFACE_ONLY
float4 SamplePrevCompositeGuide(float2 uv)
{
    uint width;
    uint height;
    PrevCompositeGuideTex.GetDimensions(width, height);
    uint2 dims = uint2(max(width, 1u), max(height, 1u));
    float2 pixel = saturate(uv) * float2(dims) - 0.5f.xx;
    float2 basePixel = floor(pixel);
    float2 f = pixel - basePixel;
    int2 baseInt = int2(basePixel);

    uint2 p00 = ClampGuidePixel(baseInt, dims);
    uint2 p10 = ClampGuidePixel(baseInt + int2(1, 0), dims);
    uint2 p01 = ClampGuidePixel(baseInt + int2(0, 1), dims);
    uint2 p11 = ClampGuidePixel(baseInt + int2(1, 1), dims);

    float4 g00 = PrevCompositeGuideTex.Load(int3(p00, 0));
    float4 g10 = PrevCompositeGuideTex.Load(int3(p10, 0));
    float4 g01 = PrevCompositeGuideTex.Load(int3(p01, 0));
    float4 g11 = PrevCompositeGuideTex.Load(int3(p11, 0));

    float w00 = (1.0f - f.x) * (1.0f - f.y) * (g00.w > 0.0001f ? 1.0f : 0.0f);
    float w10 = f.x * (1.0f - f.y) * (g10.w > 0.0001f ? 1.0f : 0.0f);
    float w01 = (1.0f - f.x) * f.y * (g01.w > 0.0001f ? 1.0f : 0.0f);
    float w11 = f.x * f.y * (g11.w > 0.0001f ? 1.0f : 0.0f);
    float guideWeight = w00 + w10 + w01 + w11;
    if (guideWeight <= 0.0001f)
        return float4(saturate(uv), 0.0f, 0.0f);

    float2 guideUv = (g00.xy * w00 + g10.xy * w10 + g01.xy * w01 + g11.xy * w11) / guideWeight;
    float guideAlpha = (g00.z * w00 + g10.z * w10 + g01.z * w01 + g11.z * w11) / guideWeight;
    return float4(saturate(guideUv), saturate(guideAlpha), saturate(guideWeight));
}
#endif

PSInput VSMain(VSInput input)
{
    PSInput result;
    float4 worldPos = mul(float4(input.position, 1.0f), WorldMatrix);
    float4 viewPos = mul(float4(input.position, 1.0f), WorldViewMatrix);
    result.position = mul(float4(input.position, 1.0f), WorldViewProjectionMatrix);
    result.viewPos = viewPos.xyz;
    result.viewNormal = normalize(mul(float4(input.normal, 0.0f), NormalWorldViewMatrix).xyz);
    result.worldPos = worldPos.xyz;
    result.worldNormal = normalize(mul(float4(input.normal, 0.0f), NormalWorldMatrix).xyz);
    return result;
}

bool ProjectWorldToScreenUv(float3 worldPos, out float2 uv)
{
    float4 clip = mul(float4(worldPos, 1.0f), ViewProjectionMatrix);
    if (clip.w <= 1.0e-5f)
    {
        uv = float2(0.0f, 0.0f);
        return false;
    }

    float2 ndc = clip.xy / clip.w;
    uv = ndc * float2(0.5f, -0.5f) + 0.5f;
    return all(uv >= float2(0.0f, 0.0f)) && all(uv <= float2(1.0f, 1.0f));
}

float3 ReflectionEnvironment(float3 reflectedDir, float reflectionScale, float grazing, float roughness)
{
    float sky = saturate(reflectedDir.y * 0.5f + 0.5f);
    float horizon = pow(saturate(1.0f - abs(reflectedDir.y)), 3.0f);
    float3 reflectedEnv = lerp(
        float3(0.018f, 0.022f, 0.030f),
        float3(0.65f, 0.78f, 0.96f),
        sky);
    reflectedEnv += float3(0.18f, 0.26f, 0.38f) * horizon * (0.35f + 0.25f * reflectionScale);

    float3 glintDir = normalize(float3(-0.35f, 0.55f, 0.76f));
    float glintPower = lerp(
        lerp(96.0f, 24.0f, saturate(reflectionScale * 0.333f)),
        4.0f,
        saturate(roughness));
    float glint = pow(saturate(dot(reflectedDir, glintDir)), glintPower);
    float3 reflected = reflectedEnv * (0.38f + 0.20f * reflectionScale + 0.42f * grazing);
    reflected += float3(1.0f, 0.96f, 0.88f) * glint *
        (0.25f + 0.35f * reflectionScale) * lerp(1.0f, 0.35f, saturate(roughness));
    return reflected;
}

#if TRANSLUCENT_ENABLE_RT_REFLECTION
float3 TranslucentSafeNormalize(float3 value, float3 fallback)
{
    float lenSq = dot(value, value);
    return lenSq > 1.0e-8f ? value * rsqrt(lenSq) : fallback;
}

bool TraceTranslucentReflectionShadowVisible(float3 hitWorld, float3 hitNormal, float3 lightDir)
{
    RayDesc shadowRay;
    shadowRay.Origin = hitWorld + hitNormal * 0.5f;
    shadowRay.Direction = lightDir;
    shadowRay.TMin = 0.0f;
    shadowRay.TMax = 100000.0f;

    RayQuery<
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
        RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
        RAY_FLAG_FORCE_OPAQUE |
        RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> shadowQuery;
    shadowQuery.TraceRayInline(gRtScene, RAY_FLAG_NONE, TRANSLUCENT_RT_MASK_OPAQUE, shadowRay);
    while (shadowQuery.Proceed())
    {
    }

    return shadowQuery.CommittedStatus() == COMMITTED_NOTHING;
}

float3 ShadeTranslucentReflectionHit(
    RayDesc ray,
    float hitT,
    float2 hitBary,
    uint triangleIndex,
    uint instanceID)
{
    hitT = max(hitT, 0.0f);
    float3 fallbackHitWorld = ray.Origin + ray.Direction * hitT;
    float3 barycentrics = float3(1.0f - hitBary.x - hitBary.y, hitBary.x, hitBary.y);

    Vertex vertex = CORONA_GET_SURFACE_VERTEX_ATTRIBUTES(instanceID, triangleIndex, barycentrics);
    float3 hitWorld = CommonSanitizeFloat3(vertex.position, fallbackHitWorld);
    float3 hitNormal = CommonSafeNormalize(vertex.normal, -ray.Direction);
    if (dot(hitNormal, -ray.Direction) < 0.0f)
        hitNormal = -hitNormal;

    RTMaterialRecord material = RtMaterials[instanceID];
    vertex.textureLODConstant += material.AlbedoLodConstant;
    float noV = max(abs(dot(hitNormal, -ray.Direction)), 1.0e-4f);
    float rayConeWidth = max(ReflectionHitLightColorAndViewSpread.w, 0.0f) * hitT;
    float mipLevel = computeTextureLOD(noV, rayConeWidth, vertex.textureLODConstant);

    float3 baseColor = max(CommonSanitizeFloat3(material.BaseColorFactor.rgb, 1.0f.xxx), 0.0f.xxx);
    if (IsValidBindlessTextureIndex(material.AlbedoTextureIndex))
    {
        float3 sampledAlbedo = MaterialTextures[NonUniformResourceIndex(material.AlbedoTextureIndex)]
            .SampleLevel(samplerClamp, vertex.uv, mipLevel).rgb;
        baseColor *= max(CommonSanitizeFloat3(sampledAlbedo, 1.0f.xxx), 0.0f.xxx);
    }

    float3 lightDir = CommonSafeNormalize(ReflectionHitLightDirAndIntensity.xyz, float3(0.0f, 1.0f, 0.0f));
    float lightIntensity = max(ReflectionHitLightDirAndIntensity.w, 0.0f);
    float ndotl = saturate(dot(hitNormal, lightDir));
    float visibility = 1.0f;
    if (lightIntensity > 0.0f && ndotl > 0.0f)
        visibility = TraceTranslucentReflectionShadowVisible(hitWorld, hitNormal, lightDir) ? 1.0f : 0.35f;

    float3 lightColor = max(ReflectionHitLightColorAndViewSpread.rgb, 0.0f.xxx);
    float3 direct = baseColor * lightColor * (ndotl * lightIntensity * visibility * (1.0f / PI));
    float hemi = saturate(hitNormal.y * 0.5f + 0.5f);
    float3 ambientColor = lerp(0.035f.xxx, max(lightColor, 0.08f.xxx) * 0.10f, hemi);
    return max(direct + baseColor * ambientColor, 0.0f.xxx);
}

float3 TraceRayReflectionColor(
    float3 worldPos,
    float3 worldNormal,
    float roughness,
    uint2 pixel,
    out bool rayHit,
    out bool usedHitLightingFallback)
{
    rayHit = false;
    usedHitLightingFallback = false;

    float3 cameraPos = CameraPositionAndRayParams.xyz;
    float3 incident = normalize(worldPos - cameraPos);
    float3 normal = normalize(worldNormal);
    float3 reflectedDir = SampleRoughReflectionDirection(
        incident,
        normal,
        roughness,
        pixel,
        StochasticParams.y,
        StochasticParams.z);

    float bias = max(CameraPositionAndRayParams.w, 0.01f);
    RayDesc ray;
    ray.Origin = worldPos + normal * bias + reflectedDir * bias;
    ray.Direction = reflectedDir;
    ray.TMin = bias;
    ray.TMax = 100000.0f;

    RayQuery<
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
        RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
        RAY_FLAG_FORCE_OPAQUE |
        RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(gRtScene, RAY_FLAG_NONE, TRANSLUCENT_RT_MASK_OPAQUE, ray);
    while (query.Proceed())
    {
    }

    if (query.CommittedStatus() != COMMITTED_NOTHING)
    {
        float3 hitWorld = ray.Origin + ray.Direction * query.CommittedRayT();
        float2 hitUv;
        if (ProjectWorldToScreenUv(hitWorld, hitUv))
        {
            rayHit = true;
            return SceneColorTex.SampleLevel(samplerClamp, saturate(hitUv), 0.0f).rgb;
        }

        if (TranslucentFlagEnabled(TRANSLUCENT_FLAG_REFLECTION_HIT_LIGHTING_FALLBACK))
        {
            rayHit = true;
            usedHitLightingFallback = true;
            return ShadeTranslucentReflectionHit(
                ray,
                query.CommittedRayT(),
                query.CommittedTriangleBarycentrics(),
                query.CommittedPrimitiveIndex(),
                query.CommittedInstanceID());
        }
    }

    return float3(0.0f, 0.0f, 0.0f);
}
#endif

struct PSOutput
{
    float4 Color : SV_Target0;
#if TRANSLUCENT_SURFACE_ONLY
    float4 Surface : SV_Target1;
#else
    float4 RefractionGuide : SV_Target1;
    float4 Surface : SV_Target2;
#endif
};

PSOutput PSMain(PSInput input)
{
    PSOutput output;

    float4 baseColor = saturate(BaseColorFactor);
    float alpha = baseColor.a;

    float2 invTargetSize = RenderTargetParams.xy;
    float2 screenUv = saturate(input.position.xy * invTargetSize);
    float3 viewNormal = normalize(input.viewNormal);
    float3 viewDir = normalize(-input.viewPos);
    if (dot(viewNormal, viewDir) < 0.0f)
        viewNormal = -viewNormal;
    float noV = saturate(dot(viewNormal, viewDir));

    float2 normalOffset = float2(viewNormal.x, -viewNormal.y);

    float grazing = 1.0f - noV;
    float refractionPixels = EffectParams.x;
    float reflectionScale = max(EffectParams.y * (1.0f / 28.0f), 0.0f);
    float reflectionStrength = saturate(EffectParams.z);
    float fresnelPower = max(EffectParams.w, 0.001f);
    float glassRoughness = saturate(GlassParams.x);
    uint2 stochasticPixel = uint2(max(input.position.xy, 0.0f.xx));

    float4 rtRefraction = RefractedColorTex.Load(int3(stochasticPixel, 0));
    bool useRtRefraction =
        TranslucentFlagEnabled(TRANSLUCENT_FLAG_RT_REFRACTION_COLOR) &&
        rtRefraction.a > 0.5f;
    float2 roughRefractionOffsetUv = float2(0.0f, 0.0f);
#if !TRANSLUCENT_SURFACE_ONLY
    if (!useRtRefraction && glassRoughness > 1.0e-4f)
    {
        float2 refractionRandom = float2(
            Random01(stochasticPixel, StochasticParams.y, StochasticParams.z ^ 0x68bc21ebu),
            Random01(stochasticPixel, StochasticParams.y, StochasticParams.z ^ 0x02e5be93u));
        float2 roughnessDisk = SampleConcentricDisk(refractionRandom);
        float roughnessRadiusPixels =
            glassRoughness * glassRoughness *
            lerp(12.0f, 48.0f, grazing);
        roughRefractionOffsetUv = roughnessDisk * roughnessRadiusPixels * invTargetSize;
    }
#endif
    float3 destinationColor = SceneColorTex.SampleLevel(samplerClamp, screenUv, 0.0f).rgb;
    float3 refracted = useRtRefraction ?
        max(rtRefraction.rgb, 0.0f.xxx) :
        SceneColorTex.SampleLevel(samplerClamp, saturate(screenUv + roughRefractionOffsetUv), 0.0f).rgb;
    float2 compositeGuideUv = screenUv;

#if !TRANSLUCENT_SURFACE_ONLY
    float rippleA = sin(dot(screenUv, float2(43.1f, 17.3f)) * 6.2831853f);
    float rippleB = cos(dot(screenUv, float2(19.7f, 53.9f)) * 6.2831853f);
    float2 rippleOffset = float2(rippleA, rippleB) * (1.0f - saturate(length(normalOffset)));
    float2 refractionDir = normalOffset * 0.85f + rippleOffset * 0.055f;
    float2 refractionUv = screenUv + refractionDir * refractionPixels * invTargetSize * alpha * (0.35f + 0.65f * grazing);
    refractionUv += roughRefractionOffsetUv;
    float2 refractionSampleUv = saturate(refractionUv);
    float4 prevCompositeGuide = SamplePrevCompositeGuide(refractionSampleUv);
    compositeGuideUv = prevCompositeGuide.w > 0.0001f ? prevCompositeGuide.xy : refractionSampleUv;
    refracted = useRtRefraction ?
        max(rtRefraction.rgb, 0.0f.xxx) :
        SceneColorTex.SampleLevel(samplerClamp, refractionSampleUv, 0.0f).rgb;
#endif

    if (StochasticParams.w != 0u)
    {
#if TRANSLUCENT_SURFACE_ONLY
        // LightingBuffer contains the sharp pre-light result. Replace it with
        // this layer's stochastic transmission sample; a zero-output early
        // return would discard raster roughness entirely in refraction-only mode.
        output.Color = float4(refracted, 1.0f);
        output.Surface = float4(0.0f, 0.0f, 0.0f, 0.0f);
#else
        output.Color = float4(refracted, 1.0f);
        output.RefractionGuide = float4(compositeGuideUv, alpha, 1.0f);
        output.Surface = float4(0.0f, 0.0f, 0.0f, 0.0f);
#endif
        return output;
    }

    float3 reflected = float3(0.0f, 0.0f, 0.0f);
    if (reflectionStrength > 0.0001f)
    {
        float3 incident = normalize(input.viewPos);
        float3 reflectedViewDir = SampleRoughReflectionDirection(
            incident,
            viewNormal,
            glassRoughness,
            stochasticPixel,
            StochasticParams.y,
            StochasticParams.z);
        reflected = ReflectionEnvironment(
            reflectedViewDir,
            reflectionScale,
            grazing,
            glassRoughness);

#if TRANSLUCENT_ENABLE_RT_REFLECTION
        float3 worldNormal = normalize(input.worldNormal);
        float3 worldViewDir = normalize(CameraPositionAndRayParams.xyz - input.worldPos);
        if (dot(worldNormal, worldViewDir) < 0.0f)
            worldNormal = -worldNormal;

        bool rtHit = false;
        bool usedHitLightingFallback = false;
        float3 rtReflected = TraceRayReflectionColor(
            input.worldPos,
            worldNormal,
            glassRoughness,
            stochasticPixel,
            rtHit,
            usedHitLightingFallback);
        if (rtHit)
        {
            // Screen-projected hits have a full scene-color sample. Offscreen
            // hit lighting is a one-light estimate without reflection guides,
            // so retain most of the stable environment estimate underneath it.
            float hitBlend = usedHitLightingFallback ?
                0.28f :
                saturate(0.72f + 0.10f * reflectionScale);
            reflected = lerp(reflected, rtReflected, hitBlend);
        }
#endif
    }

    float tintStrength = saturate(RenderTargetParams.z);
    float surfaceStrength = saturate(RenderTargetParams.w);
    float3 surfaceAlbedo = max(baseColor.rgb, float3(0.02f, 0.02f, 0.02f));
    float3 transmissionTint = lerp(
        float3(1.0f, 1.0f, 1.0f),
        surfaceAlbedo,
        tintStrength * (0.25f + 0.75f * alpha));
    reflected = lerp(reflected, reflected * max(baseColor.rgb, float3(0.08f, 0.08f, 0.08f)), tintStrength * 0.35f);

    float fresnel = pow(grazing, fresnelPower);
    float reflectionWeight = saturate(reflectionStrength * (0.10f + 0.90f * fresnel));
    float refractionEnergy = 1.0f - saturate(reflectionWeight * alpha * 0.35f);
    float3 refractedColor = refracted * transmissionTint * refractionEnergy;

    float3 keyLightDir = normalize(float3(-0.45f, 0.62f, 0.64f));
    float3 fillLightDir = normalize(float3(0.55f, 0.15f, 0.82f));
    float diffuse = 0.18f +
        saturate(dot(viewNormal, keyLightDir)) * 0.62f +
        saturate(dot(viewNormal, fillLightDir)) * 0.24f;
    float3 halfVector = normalize(keyLightDir + viewDir);
    float specular = pow(saturate(dot(viewNormal, halfVector)), 64.0f) * (0.55f + 0.45f * fresnel);
    float rim = pow(grazing, 2.0f) * 0.42f;
    float surfaceVisibility = surfaceStrength * alpha;
    float3 diffuseLighting = surfaceAlbedo * diffuse * surfaceVisibility * 0.55f;
    float3 rimLighting = lerp(surfaceAlbedo, float3(1.0f, 1.0f, 1.0f), 0.65f) * rim * surfaceVisibility;
    float3 specularLighting = float3(1.0f, 0.96f, 0.88f) * specular * surfaceVisibility;
    float reflectionVisibility = saturate(surfaceVisibility * (0.65f + 0.35f * fresnel) + alpha * surfaceStrength * 0.15f);
    float3 reflectionLighting = reflected * reflectionWeight * reflectionVisibility;

    float3 surfaceLighting = diffuseLighting + rimLighting + specularLighting + reflectionLighting;
    float3 color = refractedColor + surfaceLighting;

#if TRANSLUCENT_SURFACE_ONLY
    float viewDepth = max(-input.viewPos.z, 0.0f);
    float frontT = saturate((viewDepth - SurfaceDepthParams.x) * max(SurfaceDepthParams.y, 0.0f));
    float frontDepthWeight = pow(saturate(1.0f - frontT), max(SurfaceDepthParams.z, 0.001f));
    float depthWeight = lerp(saturate(SurfaceDepthParams.w), 1.0f, frontDepthWeight);
    float blendAlpha = saturate(alpha * depthWeight);

    // This pass runs after LightingPass and samples a copy of the current
    // LightingBuffer in SceneColorTex. If we output `color` directly with
    // standard alpha blending, many glass layers converge toward opaque
    // pastel source colors. Solve the blend equation instead so the target
    // receives the intended transmitted/tinted color for this layer. Subtract
    // the actual destination pixel, not the displaced refraction sample;
    // subtracting `refracted` would reintroduce the sharp destination through
    // the hardware (1-alpha) term and cancel most of the roughness.
    float safeBlendAlpha = max(blendAlpha, 1.0e-4f);
    float3 blendSource = (color - destinationColor * (1.0f - safeBlendAlpha)) * rcp(safeBlendAlpha);
    output.Color = float4(blendSource, blendAlpha);
    output.Surface = float4(surfaceLighting, blendAlpha);
#else
    // Refractive translucency has already sampled the background it should
    // transmit. Write an opaque composite so the original opaque surface is
    // not alpha-blended back on top of the refracted result.
    output.Color = float4(color, 1.0f);
    output.RefractionGuide = float4(compositeGuideUv, alpha, 1.0f);
    output.Surface = float4(0.0f, 0.0f, 0.0f, 0.0f);
#endif
    return output;
}
