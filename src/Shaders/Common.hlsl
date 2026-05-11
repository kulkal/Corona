#define PI 3.14159265

#define DOWNSAMPLE_SIZE 3

float3 CommonSafeNormalize(float3 value, float3 fallback)
{
    if (any(isnan(value)) || any(isinf(value)))
        value = fallback;

    float lenSq = dot(value, value);
    if (lenSq > 1e-8f)
        return value * rsqrt(lenSq);

    if (any(isnan(fallback)) || any(isinf(fallback)))
        fallback = float3(0.0f, 0.0f, 0.0f);

    float fallbackLenSq = dot(fallback, fallback);
    return fallbackLenSq > 1e-8f ? fallback * rsqrt(fallbackLenSq) : fallback;
}

float CommonSanitizeFloat(float value, float fallback)
{
    return (isnan(value) || isinf(value)) ? fallback : value;
}

float3 CommonSanitizeFloat3(float3 value, float3 fallback)
{
    if (any(isnan(value)) || any(isinf(value)))
        return fallback;
    return value;
}

float3 EnvBRDFApprox2(float3 specularColor, float alpha, float NoV)
{
    NoV = abs(NoV);

    float4 X;
    X.x = 1.0f;
    X.y = NoV;
    X.z = NoV * NoV;
    X.w = NoV * X.z;

    float4 Y;
    Y.x = 1.0f;
    Y.y = alpha;
    Y.z = alpha * alpha;
    Y.w = alpha * Y.z;

    float2x2 M1 = float2x2(0.99044f, -1.28514f, 1.29678f, -0.755907f);
    float3x3 M2 = float3x3(1.0f, 2.92338f, 59.4188f,
                            20.3225f, -27.0302f, 222.592f,
                            121.563f, 626.13f, 316.627f);
    float2x2 M3 = float2x2(0.0365463f, 3.32707f, 9.0632f, -9.04756f);
    float3x3 M4 = float3x3(1.0f, 3.59685f, -1.36772f,
                            9.04401f, -16.3174f, 9.22949f,
                            5.56589f, 19.7886f, -20.2123f);

    float biasDenom = dot(mul(M2, X.xyw), Y.xyw);
    float scaleDenom = dot(mul(M4, X.xzw), Y.xyw);
    float bias = dot(mul(M1, X.xy), Y.xy) / max(abs(biasDenom), 1.0e-6f);
    float scale = dot(mul(M3, X.xy), Y.xy) / max(abs(scaleDenom), 1.0e-6f);

    bias *= saturate(specularColor.g * 50.0f);
    return mad(specularColor, max(0.0f, scale), max(0.0f, bias));
}

float3 ComputeDLSSRRSpecularAlbedo(float3 baseColor, float metallic, float roughness, float3 normal, float3 viewDir)
{
    float3 F0 = lerp(0.04f.xxx, saturate(baseColor), saturate(metallic));
    float3 N = CommonSafeNormalize(normal, float3(0.0f, 1.0f, 0.0f));
    float3 V = CommonSafeNormalize(viewDir, N);
    float NoV = saturate(dot(N, V));
    float alpha = saturate(roughness) * saturate(roughness);
    return saturate(EnvBRDFApprox2(F0, alpha, NoV));
}


float GetLinearDepth(float DeviceDepth, float ParamX, float ParamY, float ParamZ)
{
    // return Near/(Near-Far)/(DeviceDepth -Far/(Far-Near))*Far
    return ParamY / (DeviceDepth - ParamX) * ParamZ;
}

float GetLinearDepthOpenGL(float DeviceDepth, float Near, float Far)
{
    float z_n = DeviceDepth;// * 2 - 1;

    return Near * Far /(Far + Near -z_n * (Far - Near));
}

// float3 GetViewPosition(float LinearDepth, float2 ScreenPosition, float Proj11, float Proj22)
// {
//     float2 screenSpaceRay = float2(ScreenPosition.x / Proj11,
//                                    ScreenPosition.y / Proj22);
    
//     float3 ViewPosition;
//     ViewPosition.z = LinearDepth;
//     // Solve the two projection equations
//     ViewPosition.xy = screenSpaceRay.xy * ViewPosition.z;
//     ViewPosition.z *= -1;
//     return ViewPosition;
// }

float3 GetViewPosition(float LinearDepth, float2 ScreenPosition, float4x4 InvProjMat)
{
    float4 ClipPos = float4(ScreenPosition, LinearDepth, 1);

    float4 ViewPosition = mul(ClipPos, InvProjMat);
    // ViewPosition.z = LinearDepth;
    // Solve the two projection equations
    // ViewPosition.xy = ScreenPosition.xy * ViewPosition.z;
    // ViewPosition.z *= -1;
    ViewPosition.xyz /=ViewPosition.w;
    return ViewPosition.xyz;
}

float3 SampleHemisphereCosine(float u, float v /*out float pdf*/)
{
	float3 p;
	float r = sqrt(u);
	float phi = 2.0f * PI * v;
	p.x = r * cos(phi);
	p.y = r * sin(phi);
	p.z = sqrt(1 - u);
	// pdf = p.z * (1.f / PI);
	return p;
}


float3 SampleUniformHemisphere(float u, float v)
{
	float3 p;
    float r = sqrt(1-u*u);
    float phi = 2 * PI * v;
    p.x = r*cos(phi);
    p.y = r*sin(phi);
    p.z = u;

    return p;
}

float3 SampleDirectionalLightSphereCap(float3 direction, float angularRadius, float2 u)
{
    float3 center = direction;
    float lenSq = dot(center, center);
    center = lenSq > 1e-8f ? center * rsqrt(lenSq) : float3(0.0f, 1.0f, 0.0f);

    angularRadius = clamp(angularRadius, 0.0f, 0.25f);
    float cosThetaMax = cos(angularRadius);
    float cosTheta = lerp(1.0f, cosThetaMax, saturate(u.x));
    float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    float phi = 2.0f * PI * u.y;

    float3 up = abs(center.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(up, center));
    float3 bitangent = cross(center, tangent);

    return normalize(
        tangent * (cos(phi) * sinTheta) +
        bitangent * (sin(phi) * sinTheta) +
        center * cosTheta);
}


struct SH
{
    float4 shY;
    float2 CoCg;
};

#define ENABLE_SH 1

float3 project_SH_irradiance(SH sh, float3 N)
{
#if ENABLE_SH
    float d = dot(sh.shY.xyz, N);
    float Y = 2.0 * (1.023326 * d + 0.886226 * sh.shY.w);
    Y = max(Y, 0.0);

    sh.CoCg *= Y * 0.282095 / (sh.shY.w + 1e-6);

    float   T       = Y - sh.CoCg.y * 0.5;
    float   G       = sh.CoCg.y + T;
    float   B       = T - sh.CoCg.x * 0.5;
    float   R       = B + sh.CoCg.x;

    return max(float3(R, G, B), float3(0.0, 0.0, 0.0));
#else
    return sh.shY.xyz;
#endif
}

SH irradiance_to_SH(float3 color, float3 dir)
{
    SH result;

#if ENABLE_SH
    float   Co      = color.r - color.b;
    float   t       = color.b + Co * 0.5;
    float   Cg      = color.g - t;
    float   Y       = max(t + Cg * 0.5, 0.0);

    result.CoCg = float2(Co, Cg);

    float   L00     = 0.282095;
    float   L1_1    = 0.488603 * dir.y;
    float   L10     = 0.488603 * dir.z;
    float   L11     = 0.488603 * dir.x;

    result.shY = float4 (L11, L1_1, L10, L00) * Y;
#else
    result.shY = float4(color, 0);
    result.CoCg = float2(0);
#endif

    return result;
}

float3 SH_to_irradiance(SH sh)
{
    float   Y       = sh.shY.w / 0.282095;

    float   T       = Y - sh.CoCg.y * 0.5;
    float   G       = sh.CoCg.y + T;
    float   B       = T - sh.CoCg.x * 0.5;
    float   R       = B + sh.CoCg.x;

    return max(float3(R, G, B), float3(0.0, 0.0, 0.0));
}

SH init_SH()
{
    SH result;
    result.shY = float4(0, 0, 0, 0);
    result.CoCg = float2(0, 0);
    return result;
}

void accumulate_SH(inout SH accum, SH b, float scale)
{
    accum.shY += b.shY * scale;
    accum.CoCg += b.CoCg * scale;
}

void scale_SH(inout SH sh, float scale)
{
	sh.shY *= scale;
	sh.CoCg *= scale;
}

float2 LoadBlueNoise2(Texture3D blueNoiseSource, uint2 launchIndex, uint frameCounter, uint stride)
{
    uint offset = frameCounter;
    uint3 addr = uint3(launchIndex.x % 64, launchIndex.y % 64, (offset/2) % 64);
    float4 Noise = blueNoiseSource[addr];

    if(offset % 2 == 0)
        return Noise.xy;
    else
        return Noise.zw;
}

uint HashUInt(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float HashToUnitFloat(uint x)
{
    return (HashUInt(x) >> 8) * (1.0f / 16777216.0f);
}

float2 LoadStableHashNoise2(uint2 launchIndex)
{
    uint seed = launchIndex.x * 1973u + launchIndex.y * 9277u + 0x68bc21ebu;
    return float2(HashToUnitFloat(seed), HashToUnitFloat(seed ^ 0xb5297a4du));
}

float2 LoadR2LowDiscrepancyNoise2(uint2 launchIndex, uint frameCounter)
{
    const float2 r2 = float2(0.7548776662466927f, 0.5698402909980532f);
    return frac(LoadStableHashNoise2(launchIndex) + r2 * float(frameCounter));
}

// blueNoiseSource is sampled only when noiseMode selects BLUE_NOISE.
// R2 low-discrepancy and stable-hash modes are generated analytically.
float2 GenerateRaySample2D(Texture3D blueNoiseSource, uint2 launchIndex, uint frameCounter, uint stride, uint noiseMode)
{
    if (noiseMode == 0u)
        return LoadBlueNoise2(blueNoiseSource, launchIndex, frameCounter, stride);

    if (noiseMode == 2u)
        return LoadStableHashNoise2(launchIndex);

    return LoadR2LowDiscrepancyNoise2(launchIndex, frameCounter);
}

float square(float x) { return x * x; }

float3x3 construct_ONB_frisvad(float3 normal)
{
    float3x3 ret;
    ret[1] = normal;
    if(normal.z < -0.999805696f) {
        ret[0] = float3(0.0f, -1.0f, 0.0f);
        ret[2] = float3(-1.0f, 0.0f, 0.0f);
    }
    else 
    {
        float a = 1.0f / (1.0f + normal.z);
        float b = -normal.x * normal.y * a;
        ret[0] = float3(1.0f - normal.x * normal.x * a, b, -normal.x);
        ret[2] = float3(b, 1.0f - normal.y * normal.y * a, -normal.y);
    }
    return ret;
}

float schlick_ross_fresnel(float F0, float roughness, float NdotV)
{
    if(F0 < 0)
        return 0;

    // Shlick's approximation for Ross BRDF -- makes Fresnel converge to less than 1.0 when N.V is low
    return F0 + (1 - F0) * pow(1 - NdotV, 5 * exp(-2.69 * roughness)) / (1.0 + 22.7 * pow(roughness, 1.5));
}

float G_Smith_over_NdotV(float roughness, float NdotV, float NdotL)
{
    float alpha = square(roughness);
    float g1 = NdotV * sqrt(square(alpha) + (1.0 - square(alpha)) * square(NdotL));
    float g2 = NdotL * sqrt(square(alpha) + (1.0 - square(alpha)) * square(NdotV));
    return 2.0 *  NdotL / (g1 + g2);
}

float GGX(float3 V, float3 L, float3 N, float roughness, float NoH_offset)
{
    float3 H = normalize(L - V);
    
    float NoL = max(0, dot(N, L));
    float VoH = max(0, -dot(V, H));
    float NoV = max(0, -dot(N, V));
    float NoH = clamp(dot(N, H) + NoH_offset, 0, 1);

    if (NoL > 0)
    {
        float G = G_Smith_over_NdotV(roughness, NoV, NoL);
        float alpha = square(max(roughness, 0.02));
        float D = square(alpha) / (PI * square(square(NoH) * square(alpha) + (1 - square(NoH))));

        // Incident light = SampleColor * NoL
        // Microfacet specular = D*G*F / (4*NoL*NoV)
        // F = 1, accounted for elsewhere
        // NoL = 1, accounted for in the diffuse term
        return D * G / 4;
    }

    return 0;
}

float calcTriangleArea(float3 posA, float3 posB, float3 posC)
{
    // Fundamentals of Computer Graphics, Peter Shirley (pg. 46)
    return 0.5 * length(cross((posB - posA), (posC - posA)));
}

float computeTextureLOD(in float NdotV, in float rayConeWidth, in float triangleLodConstant)
{
    // Eq. 34
    float lambda = CommonSanitizeFloat(triangleLodConstant, 0.0f);
    lambda += log2(max(abs(rayConeWidth), 1e-6f));
    //lambda += halfLog2NumTexPixels; //< Now built into triangleLodConstant
    lambda -= log2(max(abs(NdotV), 1e-4f));
    return clamp(CommonSanitizeFloat(lambda, 0.0f), 0.0f, 16.0f);
}

float computeTriangleTextureLODConstant(float triangleArea, float triangleUvArea)
{
    triangleArea = CommonSanitizeFloat(triangleArea, 0.0f);
    triangleUvArea = CommonSanitizeFloat(triangleUvArea, 0.0f);
    if (triangleArea <= 1e-8f || triangleUvArea <= 1e-12f)
        return 0.0f;

    return CommonSanitizeFloat(0.5f * log2(triangleUvArea / triangleArea), 0.0f);
}

struct Vertex
{
    float3 position;
    float3 normal;
    float2 uv;
    float3 tangent;
    float textureLODConstant;
};

// uint3 Load3x16BitIndices(uint offsetBytes)
// {
//     uint3 index;

//     // ByteAdressBuffer loads must be aligned at a 4 byte boundary.
//     // Since we need to read three 16 bit indices: { 0, 1, 2 } 
//     // aligned at a 4 byte boundary as: { 0 1 } { 2 0 } { 1 2 } { 0 1 } ...
//     // we will load 8 bytes (~ 4 indices { a b | c d }) to handle two possible index triplet layouts,
//     // based on first index's offsetBytes being aligned at the 4 byte boundary or not:
//     //  Aligned:     { 0 1 | 2 - }
//     //  Not aligned: { - 0 | 1 2 }
//     const uint dwordAlignedOffset = offsetBytes & ~3;    
//     const uint2 four16BitIndices = indices.Load2(dwordAlignedOffset);
 
//     // Aligned: { 0 1 | 2 - } => retrieve first three 16bit indices
//     if (dwordAlignedOffset == offsetBytes)
//     {
//         index.x = four16BitIndices.x & 0xffff;
//         index.y = (four16BitIndices.x >> 16) & 0xffff;
//         index.z = four16BitIndices.y & 0xffff;
//     }
//     else // Not aligned: { - 0 | 1 2 } => retrieve last three 16bit indices
//     {
//         index.x = (four16BitIndices.x >> 16) & 0xffff;
//         index.y = four16BitIndices.y & 0xffff;
//         index.z = (four16BitIndices.y >> 16) & 0xffff;
//     }

//     return index;
// }

uint3 GetIndices(ByteAddressBuffer ib, uint triangleIndex)
{
    // For 32-bit indices: each triangle uses 12 bytes (3 indices * 4 bytes)
    uint baseIndex = triangleIndex * 12;
    
    uint3 index;
    index.x = ib.Load(baseIndex);
    index.y = ib.Load(baseIndex + 4);
    index.z = ib.Load(baseIndex + 8);
    
    return index;
}

static const uint INSTANCE_PROPERTY_STRIDE = 96;
static const uint INSTANCE_PROPERTY_WORLD_MATRIX_OFFSET = 0;
static const uint INSTANCE_PROPERTY_VERTEX_OFFSET = 64;
static const uint INSTANCE_PROPERTY_INDEX_OFFSET = 68;
static const uint INSTANCE_PROPERTY_FLAGS_OFFSET = 72;
static const uint INSTANCE_PROPERTY_OVERRIDE_RM_OFFSET = 76;
static const uint INSTANCE_PROPERTY_ROUGHNESS_METALLIC_OFFSET = 80;
static const uint INSTANCE_FLAG_ALPHA_TESTED = 1u << 0;

bool IsAlphaTestedInstance(uint instanceID, ByteAddressBuffer ip)
{
    uint instancePropertyBase = instanceID * INSTANCE_PROPERTY_STRIDE;
    uint flags = ip.Load(instancePropertyBase + INSTANCE_PROPERTY_FLAGS_OFFSET);
    return (flags & INSTANCE_FLAG_ALPHA_TESTED) != 0u;
}

uint GetInstanceRoughnessMetallicOverride(uint instanceID, ByteAddressBuffer ip)
{
    uint instancePropertyBase = instanceID * INSTANCE_PROPERTY_STRIDE;
    return ip.Load(instancePropertyBase + INSTANCE_PROPERTY_OVERRIDE_RM_OFFSET);
}

float2 GetInstanceRoughnessMetallic(uint instanceID, ByteAddressBuffer ip)
{
    uint instancePropertyBase = instanceID * INSTANCE_PROPERTY_STRIDE;
    return asfloat(ip.Load2(instancePropertyBase + INSTANCE_PROPERTY_ROUGHNESS_METALLIC_OFFSET));
}

void ApplyInstanceRoughnessMetallic(uint instanceID, ByteAddressBuffer ip, inout float roughness, inout float metallic)
{
    float2 instanceRoughnessMetallic = GetInstanceRoughnessMetallic(instanceID, ip);
    instanceRoughnessMetallic = max(instanceRoughnessMetallic, float2(0.0f, 0.0f));

    if (GetInstanceRoughnessMetallicOverride(instanceID, ip) != 0u)
    {
        roughness = instanceRoughnessMetallic.x;
        metallic = instanceRoughnessMetallic.y;
    }
    else
    {
        roughness = max(roughness, 0.01f) * instanceRoughnessMetallic.x;
        metallic = max(metallic, 0.01f) * instanceRoughnessMetallic.y;
    }

    roughness = clamp(roughness, 0.02f, 1.0f);
    metallic = saturate(metallic);
}

uint3 GetIndicesWithOffset(ByteAddressBuffer ib, uint triangleIndex, uint indexOffset)
{
    uint baseIndex = (indexOffset + triangleIndex * 3) * 4;

    uint3 index;
    index.x = ib.Load(baseIndex);
    index.y = ib.Load(baseIndex + 4);
    index.z = ib.Load(baseIndex + 8);

    return index;
}

Vertex GetVertexAttributes(uint instanceID, ByteAddressBuffer vb, ByteAddressBuffer ib, ByteAddressBuffer ip, uint triangleIndex, float3 barycentrics)
{
    uint instancePropertyBase = instanceID * INSTANCE_PROPERTY_STRIDE;
    uint vertexOffset = ip.Load(instancePropertyBase + INSTANCE_PROPERTY_VERTEX_OFFSET);
    uint indexOffset = ip.Load(instancePropertyBase + INSTANCE_PROPERTY_INDEX_OFFSET);
    uint3 index = GetIndicesWithOffset(ib, triangleIndex, indexOffset) + vertexOffset;
    Vertex v;
    v.position = float3(0, 0, 0);
    v.uv = float2(0, 0);


    float3 p0 = asfloat(vb.Load3(index[0] * 44));
    float3 p1 = asfloat(vb.Load3(index[1] * 44));
    float3 p2 = asfloat(vb.Load3(index[2] * 44));
    
    // Load vertex normals from buffer (offset 12)
    float3 n0 = asfloat(vb.Load3(index[0] * 44 + 12));
    float3 n1 = asfloat(vb.Load3(index[1] * 44 + 12));
    float3 n2 = asfloat(vb.Load3(index[2] * 44 + 12));

    // InstanceProperty stores row-major object-to-world rows, matching the TLAS instance transform.
    float4x4 WorldMatrix = {
        asfloat(ip.Load4(instancePropertyBase + INSTANCE_PROPERTY_WORLD_MATRIX_OFFSET)), 
        asfloat(ip.Load4(instancePropertyBase + INSTANCE_PROPERTY_WORLD_MATRIX_OFFSET + 16)), 
        asfloat(ip.Load4(instancePropertyBase + INSTANCE_PROPERTY_WORLD_MATRIX_OFFSET + 16*2)),
        asfloat(ip.Load4(instancePropertyBase + INSTANCE_PROPERTY_WORLD_MATRIX_OFFSET + 16*3)),
    };

    v.position += p0 * barycentrics[0];
    v.position += p1 * barycentrics[1];
    v.position += p2 * barycentrics[2];

    v.position = mul(WorldMatrix, float4(v.position, 1)).xyz;

    float2 uv0 = asfloat(vb.Load2(index[0] * 44 + 24));
    float2 uv1 = asfloat(vb.Load2(index[1] * 44 + 24));
    float2 uv2 = asfloat(vb.Load2(index[2] * 44 + 24));

    v.uv += uv0 * barycentrics[0];
    v.uv += uv1 * barycentrics[1];
    v.uv += uv2 * barycentrics[2];

    // v.uv = v.position.xy;

    // Interpolate vertex normals for smooth shading
    v.normal = n0 * barycentrics[0] + n1 * barycentrics[1] + n2 * barycentrics[2];
    v.normal = CommonSafeNormalize(v.normal, float3(0.0f, 1.0f, 0.0f));
    v.normal = mul(WorldMatrix, float4(v.normal, 0)).xyz;
    v.normal = CommonSafeNormalize(v.normal, float3(0.0f, 1.0f, 0.0f));
    
    // Load vertex tangents from buffer (offset 32)
    float3 t0 = asfloat(vb.Load3(index[0] * 44 + 32));
    float3 t1 = asfloat(vb.Load3(index[1] * 44 + 32));
    float3 t2 = asfloat(vb.Load3(index[2] * 44 + 32));
    
    // Interpolate vertex tangents
    v.tangent = t0 * barycentrics[0] + t1 * barycentrics[1] + t2 * barycentrics[2];
    v.tangent = CommonSafeNormalize(v.tangent, float3(0.0f, 0.0f, 0.0f));
    v.tangent = mul(WorldMatrix, float4(v.tangent, 0)).xyz;
    v.tangent = CommonSafeNormalize(v.tangent, float3(0.0f, 0.0f, 0.0f));

    float triangleArea = calcTriangleArea(p0, p1, p2);
    
    float3 uvA = float3(uv0.x, uv0.y, 0.f);
    float3 uvB = float3(uv1.x, uv1.y, 0.f);
    float3 uvC = float3(uv2.x, uv2.y, 0.f);   
    float triangleUvArea = calcTriangleArea(uvA, uvB, uvC);
    
    v.textureLODConstant = computeTriangleTextureLODConstant(triangleArea, triangleUvArea);

    return v;
}

Vertex GetSurfaceVertexAttributes(uint instanceID, ByteAddressBuffer vb, ByteAddressBuffer ib, ByteAddressBuffer ip, uint triangleIndex, float3 barycentrics)
{
    uint instancePropertyBase = instanceID * INSTANCE_PROPERTY_STRIDE;
    uint vertexOffset = ip.Load(instancePropertyBase + INSTANCE_PROPERTY_VERTEX_OFFSET);
    uint indexOffset = ip.Load(instancePropertyBase + INSTANCE_PROPERTY_INDEX_OFFSET);
    uint3 index = GetIndicesWithOffset(ib, triangleIndex, indexOffset) + vertexOffset;
    Vertex v;
    v.position = float3(0, 0, 0);
    v.uv = float2(0, 0);
    v.tangent = float3(0, 0, 0);


    float3 p0 = asfloat(vb.Load3(index[0] * 44));
    float3 p1 = asfloat(vb.Load3(index[1] * 44));
    float3 p2 = asfloat(vb.Load3(index[2] * 44));
    
    // Load vertex normals from buffer (offset 12)
    float3 n0 = asfloat(vb.Load3(index[0] * 44 + 12));
    float3 n1 = asfloat(vb.Load3(index[1] * 44 + 12));
    float3 n2 = asfloat(vb.Load3(index[2] * 44 + 12));

    // InstanceProperty stores row-major object-to-world rows, matching the TLAS instance transform.
    float4x4 WorldMatrix = {
        asfloat(ip.Load4(instancePropertyBase + INSTANCE_PROPERTY_WORLD_MATRIX_OFFSET)), 
        asfloat(ip.Load4(instancePropertyBase + INSTANCE_PROPERTY_WORLD_MATRIX_OFFSET + 16)), 
        asfloat(ip.Load4(instancePropertyBase + INSTANCE_PROPERTY_WORLD_MATRIX_OFFSET + 16*2)),
        asfloat(ip.Load4(instancePropertyBase + INSTANCE_PROPERTY_WORLD_MATRIX_OFFSET + 16*3)),
    };

    v.position += p0 * barycentrics[0];
    v.position += p1 * barycentrics[1];
    v.position += p2 * barycentrics[2];

    v.position = mul(WorldMatrix, float4(v.position, 1)).xyz;

    float2 uv0 = asfloat(vb.Load2(index[0] * 44 + 24));
    float2 uv1 = asfloat(vb.Load2(index[1] * 44 + 24));
    float2 uv2 = asfloat(vb.Load2(index[2] * 44 + 24));

    v.uv += uv0 * barycentrics[0];
    v.uv += uv1 * barycentrics[1];
    v.uv += uv2 * barycentrics[2];

    // Interpolate vertex normals for smooth shading
    v.normal = n0 * barycentrics[0] + n1 * barycentrics[1] + n2 * barycentrics[2];
    v.normal = CommonSafeNormalize(v.normal, float3(0.0f, 1.0f, 0.0f));
    v.normal = mul(WorldMatrix, float4(v.normal, 0)).xyz;
    v.normal = CommonSafeNormalize(v.normal, float3(0.0f, 1.0f, 0.0f));

    float triangleArea = calcTriangleArea(p0, p1, p2);
    
    float3 uvA = float3(uv0.x, uv0.y, 0.f);
    float3 uvB = float3(uv1.x, uv1.y, 0.f);
    float3 uvC = float3(uv2.x, uv2.y, 0.f);   
    float triangleUvArea = calcTriangleArea(uvA, uvB, uvC);
    
    v.textureLODConstant = computeTriangleTextureLODConstant(triangleArea, triangleUvArea);

    return v;
}

float RGBToLuminance( float3 x )
{
    return dot( x, float3(0.212671, 0.715160, 0.072169) );        // Defined by sRGB/Rec.709 gamut
}

float PointPlaneDist(float4 plane, float3 p)
{
    float3 n = plane.xyz;
    float d = plane.w;
    return (dot(n, p) + d)/length(n);
}
