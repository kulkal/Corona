#define PI 3.14159265

#define DOWNSAMPLE_SIZE 3


float GetLinearDepth(float DeviceDepth, float ParamX, float ParamY, float ParamZ)
{
    // return Near/(Near-Far)/(DeviceDepth -Far/(Far-Near))*Far
    return ParamY / (DeviceDepth - ParamX) * ParamZ;
}

float GetLinearDepthOpenGL(float DeviceDepth, float Near, float Far)
{
    float z_n = DeviceDepth;// * 2 - 1;

    return 2.0 * Near * Far /(Far + Near -z_n * (Far - Near));
}

float3 GetViewPosition(float LinearDepth, float2 ScreenPosition, float Proj11, float Proj22)
{
    float2 screenSpaceRay = float2(ScreenPosition.x / Proj11,
                                   ScreenPosition.y / Proj22);
    
    float3 ViewPosition;
    ViewPosition.z = LinearDepth;
    // Solve the two projection equations
    ViewPosition.xy = screenSpaceRay.xy * ViewPosition.z;
    ViewPosition.z *= -1;
    return ViewPosition;
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

float2 LoadBlueNoise2(Texture3D blueNoiseTex, uint2 launchIndex, uint frameCounter, uint stride)
{
    uint offset = frameCounter;
    uint3 addr = uint3(launchIndex.x % 64, launchIndex.y % 64, (offset/2) % 64);
    float4 Noise = blueNoiseTex[addr];

    if(offset % 2 == 0)
        return Noise.xy;
    else
        return Noise.zw;
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