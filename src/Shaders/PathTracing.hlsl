#include "Common.hlsl"

RWTexture2D<float4> OutputColor : register(u0);

RaytracingAccelerationStructure gRtScene : register(t0);
ByteAddressBuffer vertices : register(t1);
ByteAddressBuffer indices : register(t2);
ByteAddressBuffer InstanceProperty : register(t3);
Texture3D BlueNoiseTex : register(t4);
Texture2D AlbedoTex : register(t5);
Texture2D NormalTex : register(t6);
Texture2D RoughnessTex : register(t7);
Texture2D MetallicTex : register(t8);

cbuffer ViewParameter : register(b0)
{
    float4x4 ViewMatrix;
    float4x4 InvViewMatrix;
    float4x4 ProjMatrix;
    float4x4 InvProjMatrix;
    float4 ProjectionParams;
    float4 LightDirAndIntensity;
    float2 RandomOffset;
    uint FrameCounter;
    uint BlueNoiseOffsetStride;
    uint MaxBounces;
    uint SamplesPerPixel;
    float ViewSpreadAngle;
    uint DebugMode; // 0=None, 1=Albedo, 2=Normal, 3=Roughness, 4=Metallic, 5=WorldPos, 6=Barycentric, 7=UV, 8=InstanceID, 9=TriangleIndex
    float3 SkyColorTop;
    float SkyIntensity;
    float3 SkyColorBottom;
    float _padding;
    float3 LightColor;
    float _padding2;
};

SamplerState sampleWrap : register(s0);

struct PathTracingPayload
{
    float3 radiance;
    float3 throughput;
    float3 origin;
    float3 direction;
    uint depth;
    uint seed;
    bool done;
    
    // Debug visualization data (only for primary hit)
    float3 debugAlbedo;
    float3 debugNormal;
    float3 debugWorldPos;
    float debugRoughness;
    float debugMetallic;
    float3 debugBarycentric;
    float2 debugUV;
    uint debugInstanceID;
    uint debugTriangleIndex;
};

struct ShadowRayPayload
{
    bool bHit;
};

// Random number generation using PCG
uint pcg_hash(uint input)
{
    uint state = input * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float random_float(inout uint seed)
{
    seed = pcg_hash(seed);
    return float(seed) / 4294967296.0;
}

float3 random_in_unit_sphere(inout uint seed)
{
    float z = random_float(seed) * 2.0 - 1.0;
    float a = random_float(seed) * 2.0 * PI;
    float r = sqrt(1.0 - z * z);
    float x = r * cos(a);
    float y = r * sin(a);
    return float3(x, y, z);
}

float3 random_cosine_direction(inout uint seed)
{
    float r1 = random_float(seed);
    float r2 = random_float(seed);
    float z = sqrt(1.0 - r2);
    
    float phi = 2.0 * PI * r1;
    float x = cos(phi) * sqrt(r2);
    float y = sin(phi) * sqrt(r2);
    
    return float3(x, y, z);
}

// Schlick approximation for Fresnel
float schlick_fresnel(float cosine, float ref_idx)
{
    float r0 = (1.0 - ref_idx) / (1.0 + ref_idx);
    r0 = r0 * r0;
    return r0 + (1.0 - r0) * pow(1.0 - cosine, 5.0);
}

// Sample GGX distribution
float3 sample_ggx(float3 N, float roughness, inout uint seed)
{
    float a = roughness * roughness;
    
    float r1 = random_float(seed);
    float r2 = random_float(seed);
    
    float phi = 2.0 * PI * r1;
    float cosTheta = sqrt((1.0 - r2) / (1.0 + (a * a - 1.0) * r2));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    
    float3 H;
    H.x = sinTheta * cos(phi);
    H.y = sinTheta * sin(phi);
    H.z = cosTheta;
    
    float3x3 TBN = construct_ONB_frisvad(N);
    return normalize(mul(H, TBN));
}

[shader("raygeneration")]
void PathTracingRayGen()
{
    uint3 launchIndex = DispatchRaysIndex();
    uint3 launchDim = DispatchRaysDimensions();
    
    float2 pixelCenter = float2(launchIndex.xy) + float2(0.5, 0.5);
    float2 inUV = pixelCenter / float2(launchDim.xy);
    float2 d = inUV * 2.0 - 1.0;
    
    float aspectRatio = float(launchDim.x) / float(launchDim.y);
    
    // Initialize random seed
    // In debug mode, use fixed seed to avoid noise
    uint frameCounter = (DebugMode > 0) ? 0 : FrameCounter;
    uint seed = (launchIndex.x * 1973 + launchIndex.y * 9277 + frameCounter * 26699) | 1;
    
    // Add jitter for antialiasing if accumulating (but not in debug mode)
    if (DebugMode == 0)
    {
        float2 jitter = float2(random_float(seed), random_float(seed)) - 0.5;
        d += jitter / float2(launchDim.xy);
    }
    
    d.y = -d.y;
    d *= tan(0.8 / 2.0);
    d.x *= aspectRatio;
    
    float3 rayDir = normalize(float3(d.x, d.y, -1.0));
    rayDir = normalize(mul(float4(rayDir, 0), InvViewMatrix).xyz);
    
    float3 rayOrigin = mul(float4(0, 0, 0, 1), InvViewMatrix).xyz;
    
    // Path tracing loop
    float3 radiance = float3(0, 0, 0);
    float3 throughput = float3(1, 1, 1);
    
    for (uint bounce = 0; bounce < MaxBounces; bounce++)
    {
        RayDesc ray;
        ray.Origin = rayOrigin;
        ray.Direction = rayDir;
        ray.TMin = 0.0001;
        ray.TMax = 100000;
        
        PathTracingPayload payload;
        payload.radiance = float3(0, 0, 0);
        payload.throughput = throughput;
        payload.origin = rayOrigin;
        payload.direction = rayDir;
        payload.depth = bounce;
        payload.seed = seed;
        payload.done = false;
        payload.debugAlbedo = float3(0, 0, 0);
        payload.debugNormal = float3(0, 0, 0);
        payload.debugWorldPos = float3(0, 0, 0);
        payload.debugRoughness = 0;
        payload.debugMetallic = 0;
        payload.debugUV = float2(0, 0);
        payload.debugInstanceID = 0;
        payload.debugTriangleIndex = 0;
        
        TraceRay(gRtScene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);
        
        seed = payload.seed;
        
        // Debug visualization for primary hit only
        if (DebugMode > 0 && bounce == 0)
        {
            if (!payload.done) // Only visualize if we hit something
            {
                if (DebugMode == 1) // Albedo
                    radiance = payload.debugAlbedo;
                else if (DebugMode == 2) // Normal (remap from [-1,1] to [0,1])
                    radiance = payload.debugNormal * 0.5 + 0.5;
                else if (DebugMode == 3) // Roughness
                    radiance = float3(payload.debugRoughness, payload.debugRoughness, payload.debugRoughness);
                else if (DebugMode == 4) // Metallic
                    radiance = float3(payload.debugMetallic, payload.debugMetallic, payload.debugMetallic);
                else if (DebugMode == 5) // World Position (adjust scale for better visualization)
                {
                    // Grid pattern with 10 unit spacing
                    radiance = frac(payload.debugWorldPos * 0.1);
                }
                else if (DebugMode == 6) // Barycentric coordinates
                {
                    radiance = payload.debugBarycentric;
                }
                else if (DebugMode == 7) // UV
                {
                    radiance = float3(payload.debugUV, 0);
                }
                else if (DebugMode == 8) // InstanceID
                {
                    // Visualize instance ID as a color (modulo 8 for variety)
                    uint id = payload.debugInstanceID % 8;
                    float3 colors[8] = {
                        float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1), float3(1, 1, 0),
                        float3(1, 0, 1), float3(0, 1, 1), float3(1, 1, 1), float3(0.5, 0.5, 0.5)
                    };
                    radiance = colors[id];
                }
                else if (DebugMode == 9) // Triangle Index
                {
                    // Visualize triangle index with a gradient
                    float t = frac(float(payload.debugTriangleIndex) * 0.01);
                    radiance = float3(t, t, t);
                }
            }
            else
            {
                // Hit sky - show gray background for debug modes
                radiance = float3(0.5, 0.5, 0.5);
            }
            
            break; // Stop after first hit when debugging
        }
        
        // Always accumulate radiance from this bounce
        radiance += payload.radiance * throughput;
        
        if (payload.done)
        {
            // Hit sky - stop tracing
            break;
        }
        
        // Update for next bounce
        rayOrigin = payload.origin;
        rayDir = payload.direction;
        throughput = payload.throughput;
        
        // Validate throughput
        if (any(isnan(throughput)) || any(isinf(throughput)) || all(throughput <= 0.0))
        {
            break;
        }
        
        // Russian roulette
        if (bounce > 2)
        {
            float p = max(throughput.x, max(throughput.y, throughput.z));
            
            // If throughput is too small, terminate
            if (p < 0.001)
                break;
                
            if (random_float(seed) > p)
                break;
                
            throughput /= max(p, 0.001); // Avoid division by very small numbers
        }
    }
    
    // Validate radiance (check for NaN/Inf)
    if (any(isnan(radiance)) || any(isinf(radiance)))
    {
        radiance = float3(0, 0, 0);
    }
    
    // Accumulate with previous frames
    float3 finalColor;
    
    // In debug mode, don't accumulate - just show current frame
    if (FrameCounter == 0 || DebugMode > 0)
    {
        // First frame or debug mode - use only current radiance
        finalColor = radiance;
    }
    else
    {
        // Progressive accumulation: (prev * N + current) / (N + 1)
        float3 prevColor = OutputColor[launchIndex.xy].xyz;
        
        // Validate previous color
        if (any(isnan(prevColor)) || any(isinf(prevColor)))
        {
            prevColor = float3(0, 0, 0);
        }
        
        float n = float(FrameCounter);
        finalColor = (prevColor * n + radiance) / (n + 1.0);
    }
    
    // Final validation
    if (any(isnan(finalColor)) || any(isinf(finalColor)))
    {
        finalColor = float3(0, 0, 0);
    }
    
    OutputColor[launchIndex.xy] = float4(finalColor, 1.0);
}

[shader("closesthit")]
void PathTracingClosestHit(inout PathTracingPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    float3 barycentrics = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y, 
                                  attribs.barycentrics.x, 
                                  attribs.barycentrics.y);
    
    uint triangleIndex = PrimitiveIndex();
    uint instanceID = InstanceID();
    Vertex vertex = GetVertexAttributes(instanceID, vertices, indices, InstanceProperty, triangleIndex, barycentrics);
    
    // Get material properties
    float3 albedo = AlbedoTex.SampleLevel(sampleWrap, vertex.uv, 0).xyz;
    float roughness = RoughnessTex.SampleLevel(sampleWrap, vertex.uv, 0).x;
    float metallic = MetallicTex.SampleLevel(sampleWrap, vertex.uv, 0).x;
    
    // Store debug information for primary hit (depth == 0)
    if (payload.depth == 0)
    {
        payload.debugAlbedo = albedo;
        payload.debugRoughness = roughness;
        payload.debugMetallic = metallic;
        payload.debugWorldPos = vertex.position;
        // Store barycentric coordinates directly (R=v0 weight, G=v1 weight, B=v2 weight)
        payload.debugBarycentric = barycentrics;
        payload.debugUV = vertex.uv;
        payload.debugInstanceID = instanceID;
        payload.debugTriangleIndex = triangleIndex;
    }
    
    // Sample normal map and transform to world space
    float3 normalMap = NormalTex.SampleLevel(sampleWrap, vertex.uv, 0).xyz;
    normalMap = normalMap * 2.0 - 1.0; // Unpack from [0,1] to [-1,1]
    
    // Build TBN matrix
    float3 N = normalize(vertex.normal);
    float3 T = normalize(vertex.tangent);
    T = normalize(T - dot(T, N) * N); // Gram-Schmidt orthogonalization
    float3 B = cross(N, T);
    float3x3 TBN = float3x3(T, B, N);
    
    // Transform normal from tangent space to world space
    N = normalize(mul(normalMap, TBN));
    
    // Store final normal for debug visualization
    if (payload.depth == 0)
    {
        payload.debugNormal = N;
    }
    
    float3 V = -payload.direction;
    
    // Offset ray origin to avoid self-intersection
    // Use larger offset for path tracing to prevent shadow acne
    float3 hitPos = vertex.position + N * 0.01;
    
    // Direct lighting - sample light
    float3 lightDir = LightDirAndIntensity.xyz;
    float lightIntensity = LightDirAndIntensity.w;
    
    float3 directLight = float3(0, 0, 0);
    
    // Shadow ray
    RayDesc shadowRay;
    shadowRay.Origin = hitPos;
    shadowRay.Direction = lightDir;
    shadowRay.TMin = 0.01;  // Larger offset to prevent self-intersection
    shadowRay.TMax = 100000;
    
    ShadowRayPayload shadowPayload;
    shadowPayload.bHit = true;
    TraceRay(gRtScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 
             0xFF, 0, 0, 1, shadowRay, shadowPayload);
    
    if (!shadowPayload.bHit)
    {
        float NdotL = max(0, dot(N, lightDir));
        
        if (NdotL > 0)
        {
            // Diffuse lighting (Lambertian)
            float3 diffuse = albedo * (1.0 - metallic);
            
            // Specular BRDF (simplified Blinn-Phong)
            float3 H = normalize(lightDir + V);
            float NdotH = max(0, dot(N, H));
            float spec = pow(NdotH, (1.0 - roughness) * 128.0);
            float3 specular = lerp(float3(0.04, 0.04, 0.04), albedo, metallic) * spec;
            
            // Combine diffuse and specular with light color
            directLight = (diffuse + specular) * NdotL * lightIntensity * LightColor;
        }
    }
    
    // Set direct lighting contribution
    payload.radiance = directLight;
    
    // Indirect lighting - sample BRDF
    float3 newDir;
    
    // Calculate Fresnel for view direction
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float NdotV = max(0, dot(N, V));
    float3 F = F0 + (1.0 - F0) * pow(1.0 - NdotV, 5.0);
    
    // Choose between diffuse and specular based on Fresnel
    float specularWeight = (F.x + F.y + F.z) / 3.0;
    float diffuseWeight = 1.0 - specularWeight;
    
    // Adjust weights based on roughness (rougher = more diffuse)
    specularWeight *= (1.0 - roughness * 0.5);
    diffuseWeight = 1.0 - specularWeight;
    
    float r = random_float(payload.seed);
    
    if (r < specularWeight)
    {
        // Specular bounce
        float3 H = sample_ggx(N, roughness, payload.seed);
        newDir = reflect(-V, H);
        
        // Make sure we're above the surface
        if (dot(newDir, N) < 0)
            newDir = reflect(newDir, N);
        
        // For specular bounce, throughput is just Fresnel (already in weight)
        payload.throughput *= F;
    }
    else
    {
        // Diffuse bounce - cosine weighted sampling
        float3 localDir = random_cosine_direction(payload.seed);
        float3x3 TBN = construct_ONB_frisvad(N);
        newDir = normalize(mul(localDir, TBN));
        
        // Lambertian BRDF: albedo for diffuse materials, reduced for metals
        payload.throughput *= albedo * (1.0 - metallic);
    }
    
    payload.origin = hitPos;
    payload.direction = newDir;
    payload.done = false;
}

[shader("miss")]
void PathTracingMiss(inout PathTracingPayload payload)
{
    // Sky color - gradient based on view direction
    float t = 0.5 * (payload.direction.y + 1.0);
    float3 skyColor = lerp(SkyColorBottom, SkyColorTop, t);
    
    // Boost indirect sky contribution for better visibility on dark surfaces
    // Primary rays (depth 0) use normal intensity, indirect rays get boosted
    float indirectBoost = (payload.depth == 0) ? 1.0 : 2.0;
    
    payload.radiance = skyColor * SkyIntensity * indirectBoost;
    payload.done = true;
}

[shader("miss")]
void ShadowMiss(inout ShadowRayPayload payload)
{
    payload.bHit = false;
}

[shader("anyhit")]
void PathTracingAnyHit(inout PathTracingPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    float3 barycentrics = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y, 
                                  attribs.barycentrics.x, 
                                  attribs.barycentrics.y);
    
    uint triangleIndex = PrimitiveIndex();
    uint instanceID = InstanceID();
    Vertex vertex = GetVertexAttributes(instanceID, vertices, indices, InstanceProperty, triangleIndex, barycentrics);
    
    // Sample albedo alpha channel
    float alpha = AlbedoTex.SampleLevel(sampleWrap, vertex.uv, 0).w;
    
    // If alpha is too low, ignore this hit and continue ray traversal
    if (alpha < 0.1)
    {
        IgnoreHit();
    }
}
