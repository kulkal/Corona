#define PI 3.14159265

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