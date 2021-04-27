/*
Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.

NVIDIA CORPORATION and its licensors retain all intellectual property
and proprietary rights in and to this software, related documentation
and any modifications thereto. Any use, reproduction, disclosure or
distribution of this software and related documentation without an express
license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

//=================================================================================================================================
// PUBLIC
//=================================================================================================================================

// This function can be tuned at any time (NRD doesn't use NRD_FrontEnd_PackSpecular and NRD_BackEnd_UnpackSpecular internally)
float NRD_GetColorCompressionExposure( float roughness )
{
    // http://fooplot.com/#W3sidHlwZSI6MCwiZXEiOiIwLjUvKDErNTAqeCkiLCJjb2xvciI6IiMwMDAwMDAifSx7InR5cGUiOjAsImVxIjoiMC41KigxLXgpLygxKzYwKngpIiwiY29sb3IiOiIjMzNFRDBFIn0seyJ0eXBlIjoxMDAwLCJ3aW5kb3ciOlsiMCIsIjEiLCIwIiwiMC41Il0sInNpemUiOlsxNTAwLDY1MF19XQ--

    // OFF - better keep it on for low RPP and bad IS in use...
    // return 0;

    // OLD
    // return 0.5 / ( 1.0 + 50.0 * roughness );

    // NEW
    return 0.5 * ( 1.0 - roughness ) / ( 1.0 + 60.0 * roughness );
}

//=================================================================================================================================
// PRIVATE
//=================================================================================================================================

// Do not touch these - NRD recompilcation is needed
#define NRD_EPS                         0.01
#define NRD_FP16_VIEWZ_SCALE            0.0125
#define NRD_FP16_MAX                    65504.0
#define NRD_SH                          1

float3 _NRD_LinearToYCoCg( float3 color )
{
    float Co = color.x - color.z;
    float t = color.z + Co * 0.5;
    float Cg = color.y - t;
    float Y = t + Cg * 0.5;

    return float3( Y, Co, Cg );
}

float3 _NRD_YCoCgToLinear( float3 color )
{
    float t = color.x - color.z * 0.5;
    float g = color.z + t;
    float b = t - color.y * 0.5;
    float r = b + color.y;
    float3 res = float3( r, g, b );

    // Ignore negative values (minor deviations are possible)
    res = max( res, 0 );

    return res;
}

// IMPORTANT: diffuse NRD output is already unpacked, there is no need to unpack it! But the function below can be useful
// if you need to unpack packed NRD input in one of your own passes (for example, to show noisy input as is)

float4 _NRD_BackEnd_UnpackDiffuse( float4 diffA, float4 diffB, float3 normal, bool rgb = true )
{
    float3 c1 = diffA.xyz;
    float c0 = max( diffB.x, 1e-6 );
    float2 CoCg = diffB.yz;

    float d = dot( c1, normal );
    float Y = 2.0 * ( 1.023326 * d + 0.886226 * c0 );

    // Ignore negative values (minor deviations are possible)
    Y = max( Y, 0 );

    float modifier = 0.282095 * Y * rcp( c0 );
    CoCg *= saturate( modifier );

    #if( NRD_SH == 0 )
        Y = c0 / 0.282095;
    #endif

    float4 color;
    color.xyz = float3( Y, CoCg );
    color.w = diffA.w;

    if( rgb )
        color.xyz = _NRD_YCoCgToLinear( color.xyz );

    return color;
}

//=================================================================================================================================
// BACK-END UNPACKING
//=================================================================================================================================

float4 NRD_BackEnd_UnpackSpecular( float4 color, float roughness, bool rgb = true )
{
    float exposure = NRD_GetColorCompressionExposure( roughness );
    float k = color.x * exposure;
    color.xyz /= max( 1.0, k ) - k + NRD_EPS;

    if( rgb )
        color.xyz = _NRD_YCoCgToLinear( color.xyz );

    return color;
}

//=================================================================================================================================
// FRONT-END PACKING
//=================================================================================================================================

/*
IMPORTANT:
About "radiance" in packing functions:
- radiance != irradiance, it's pure energy coming from a particular direction
- radiance should not include any BRDFs
- radiance should be premultiplied with "exposure" (or something close to it)
- radiance should not include PI for diffuse (it will be canceled out later when denoised output will be multipolied with albedo / PI)
- use COS-distribution (or better) for diffuse rays
- use VNDF sampling for specular rays
*/

// IMPORTANT: Must be used to "clear" INF pixels
#define NRD_INF_DIFF_B                  float4( 0, 0, 0, NRD_FP16_MAX )
#define NRD_INF_SHADOW                  float2( NRD_FP16_MAX, NRD_FP16_MAX )

void NRD_FrontEnd_PackDiffuse( float3 radiance, float3 direction, float viewZ, float normHitDist, out float4 diffA, out float4 diffB )
{
    radiance = _NRD_LinearToYCoCg( radiance );

    float c0 = 0.282095 * radiance.x;
    float3 c1 = 0.488603 * radiance.x * direction;

    diffA.xyz = c1;
    diffA.w = normHitDist;

    diffB.x = c0;
    diffB.yz = radiance.yz;
    diffB.w = clamp( viewZ * NRD_FP16_VIEWZ_SCALE, -NRD_FP16_MAX, NRD_FP16_MAX );
}

float4 NRD_FrontEnd_PackSpecular( float3 radiance, float roughness, float normHitDist )
{
    float4 r;
    r.xyz = _NRD_LinearToYCoCg( radiance );
    r.w = normHitDist;

    float exposure = NRD_GetColorCompressionExposure( roughness );
    float k = r.x * exposure;
    r.xyz /= 1.0 + k + NRD_EPS;

    return r;
}

// IMPORTANT: shadow denoising assumes:
// distanceToOccluder:
// - NoL <= 0       - 0 (it's very important)
// - NoL > 0 (hit)  - hit distance
// - NoL > 0 (miss) - NRD_FP16_MAX
float2 NRD_FrontEnd_PackShadow( float viewZ, float distanceToOccluder )
{
    float2 r;
    r.x = 0.0;
    r.y = clamp( viewZ * NRD_FP16_VIEWZ_SCALE, -NRD_FP16_MAX, NRD_FP16_MAX );

    [flatten]
    if( distanceToOccluder == NRD_FP16_MAX )
        r.x = NRD_FP16_MAX;
    else if( distanceToOccluder != 0.0 )
        r.x = clamp( distanceToOccluder * NRD_FP16_VIEWZ_SCALE, 0.0001, 32768.0 );

    return r;
}
