#pragma once

#include <memory>
#include <string>
#include <vector>
#include <list>
#include <optional>
#include <glm/glm.hpp>
#include <Windows.h>
#include "pix3.h"


typedef unsigned int UINT;
typedef int BOOL;
typedef unsigned char       UINT8, * PUINT8;


#define DEFINE_ENUM_FLAG_OPERATORS2(ENUMTYPE) \
inline ENUMTYPE operator | (ENUMTYPE a, ENUMTYPE b){return (ENUMTYPE)((int)a | (int)b);} \
inline ENUMTYPE& operator |= (ENUMTYPE& a, ENUMTYPE b){return (ENUMTYPE&)((int&)a |= (int)b);} \
inline ENUMTYPE operator & (ENUMTYPE a, ENUMTYPE b){return (ENUMTYPE)((int)a & (int)b);} \
inline ENUMTYPE& operator &= (ENUMTYPE& a, ENUMTYPE b){return (ENUMTYPE&)((int&)a &= (int)b);} \
inline ENUMTYPE operator ~ (ENUMTYPE a) { return (ENUMTYPE)(~(int)a); } \
inline ENUMTYPE operator ^ (ENUMTYPE a, ENUMTYPE b){return (ENUMTYPE)((int)a ^ (int)b);} \
inline ENUMTYPE& operator ^= (ENUMTYPE& a, ENUMTYPE b){return (ENUMTYPE&)((int&)a ^= (int)b);} 



enum FILTER
{
    FILTER_MIN_MAG_MIP_POINT = 0,
    FILTER_MIN_MAG_POINT_MIP_LINEAR = 0x1,
    FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT = 0x4,
    FILTER_MIN_POINT_MAG_MIP_LINEAR = 0x5,
    FILTER_MIN_LINEAR_MAG_MIP_POINT = 0x10,
    FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR = 0x11,
    FILTER_MIN_MAG_LINEAR_MIP_POINT = 0x14,
    FILTER_MIN_MAG_MIP_LINEAR = 0x15,
    FILTER_ANISOTROPIC = 0x55,
    FILTER_COMPARISON_MIN_MAG_MIP_POINT = 0x80,
    FILTER_COMPARISON_MIN_MAG_POINT_MIP_LINEAR = 0x81,
    FILTER_COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT = 0x84,
    FILTER_COMPARISON_MIN_POINT_MAG_MIP_LINEAR = 0x85,
    FILTER_COMPARISON_MIN_LINEAR_MAG_MIP_POINT = 0x90,
    FILTER_COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR = 0x91,
    FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT = 0x94,
    FILTER_COMPARISON_MIN_MAG_MIP_LINEAR = 0x95,
    FILTER_COMPARISON_ANISOTROPIC = 0xd5,
    FILTER_MINIMUM_MIN_MAG_MIP_POINT = 0x100,
    FILTER_MINIMUM_MIN_MAG_POINT_MIP_LINEAR = 0x101,
    FILTER_MINIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT = 0x104,
    FILTER_MINIMUM_MIN_POINT_MAG_MIP_LINEAR = 0x105,
    FILTER_MINIMUM_MIN_LINEAR_MAG_MIP_POINT = 0x110,
    FILTER_MINIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR = 0x111,
    FILTER_MINIMUM_MIN_MAG_LINEAR_MIP_POINT = 0x114,
    FILTER_MINIMUM_MIN_MAG_MIP_LINEAR = 0x115,
    FILTER_MINIMUM_ANISOTROPIC = 0x155,
    FILTER_MAXIMUM_MIN_MAG_MIP_POINT = 0x180,
    FILTER_MAXIMUM_MIN_MAG_POINT_MIP_LINEAR = 0x181,
    FILTER_MAXIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT = 0x184,
    FILTER_MAXIMUM_MIN_POINT_MAG_MIP_LINEAR = 0x185,
    FILTER_MAXIMUM_MIN_LINEAR_MAG_MIP_POINT = 0x190,
    FILTER_MAXIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR = 0x191,
    FILTER_MAXIMUM_MIN_MAG_LINEAR_MIP_POINT = 0x194,
    FILTER_MAXIMUM_MIN_MAG_MIP_LINEAR = 0x195,
    FILTER_MAXIMUM_ANISOTROPIC = 0x1d5
};

enum TEXTURE_ADDRESS_MODE
{
    TEXTURE_ADDRESS_MODE_WRAP = 1,
    TEXTURE_ADDRESS_MODE_MIRROR = 2,
    TEXTURE_ADDRESS_MODE_CLAMP = 3,
    TEXTURE_ADDRESS_MODE_BORDER = 4,
    TEXTURE_ADDRESS_MODE_MIRROR_ONCE = 5
};



typedef enum FORMAT
{
    FORMAT_UNKNOWN = 0,
    FORMAT_R32G32B32A32_TYPELESS = 1,
    FORMAT_R32G32B32A32_FLOAT = 2,
    FORMAT_R32G32B32A32_UINT = 3,
    FORMAT_R32G32B32A32_SINT = 4,
    FORMAT_R32G32B32_TYPELESS = 5,
    FORMAT_R32G32B32_FLOAT = 6,
    FORMAT_R32G32B32_UINT = 7,
    FORMAT_R32G32B32_SINT = 8,
    FORMAT_R16G16B16A16_TYPELESS = 9,
    FORMAT_R16G16B16A16_FLOAT = 10,
    FORMAT_R16G16B16A16_UNORM = 11,
    FORMAT_R16G16B16A16_UINT = 12,
    FORMAT_R16G16B16A16_SNORM = 13,
    FORMAT_R16G16B16A16_SINT = 14,
    FORMAT_R32G32_TYPELESS = 15,
    FORMAT_R32G32_FLOAT = 16,
    FORMAT_R32G32_UINT = 17,
    FORMAT_R32G32_SINT = 18,
    FORMAT_R32G8X24_TYPELESS = 19,
    FORMAT_D32_FLOAT_S8X24_UINT = 20,
    FORMAT_R32_FLOAT_X8X24_TYPELESS = 21,
    FORMAT_X32_TYPELESS_G8X24_UINT = 22,
    FORMAT_R10G10B10A2_TYPELESS = 23,
    FORMAT_R10G10B10A2_UNORM = 24,
    FORMAT_R10G10B10A2_UINT = 25,
    FORMAT_R11G11B10_FLOAT = 26,
    FORMAT_R8G8B8A8_TYPELESS = 27,
    FORMAT_R8G8B8A8_UNORM = 28,
    FORMAT_R8G8B8A8_UNORM_SRGB = 29,
    FORMAT_R8G8B8A8_UINT = 30,
    FORMAT_R8G8B8A8_SNORM = 31,
    FORMAT_R8G8B8A8_SINT = 32,
    FORMAT_R16G16_TYPELESS = 33,
    FORMAT_R16G16_FLOAT = 34,
    FORMAT_R16G16_UNORM = 35,
    FORMAT_R16G16_UINT = 36,
    FORMAT_R16G16_SNORM = 37,
    FORMAT_R16G16_SINT = 38,
    FORMAT_R32_TYPELESS = 39,
    FORMAT_D32_FLOAT = 40,
    FORMAT_R32_FLOAT = 41,
    FORMAT_R32_UINT = 42,
    FORMAT_R32_SINT = 43,
    FORMAT_R24G8_TYPELESS = 44,
    FORMAT_D24_UNORM_S8_UINT = 45,
    FORMAT_R24_UNORM_X8_TYPELESS = 46,
    FORMAT_X24_TYPELESS_G8_UINT = 47,
    FORMAT_R8G8_TYPELESS = 48,
    FORMAT_R8G8_UNORM = 49,
    FORMAT_R8G8_UINT = 50,
    FORMAT_R8G8_SNORM = 51,
    FORMAT_R8G8_SINT = 52,
    FORMAT_R16_TYPELESS = 53,
    FORMAT_R16_FLOAT = 54,
    FORMAT_D16_UNORM = 55,
    FORMAT_R16_UNORM = 56,
    FORMAT_R16_UINT = 57,
    FORMAT_R16_SNORM = 58,
    FORMAT_R16_SINT = 59,
    FORMAT_R8_TYPELESS = 60,
    FORMAT_R8_UNORM = 61,
    FORMAT_R8_UINT = 62,
    FORMAT_R8_SNORM = 63,
    FORMAT_R8_SINT = 64,
    FORMAT_A8_UNORM = 65,
    FORMAT_R1_UNORM = 66,
    FORMAT_R9G9B9E5_SHAREDEXP = 67,
    FORMAT_R8G8_B8G8_UNORM = 68,
    FORMAT_G8R8_G8B8_UNORM = 69,
    FORMAT_BC1_TYPELESS = 70,
    FORMAT_BC1_UNORM = 71,
    FORMAT_BC1_UNORM_SRGB = 72,
    FORMAT_BC2_TYPELESS = 73,
    FORMAT_BC2_UNORM = 74,
    FORMAT_BC2_UNORM_SRGB = 75,
    FORMAT_BC3_TYPELESS = 76,
    FORMAT_BC3_UNORM = 77,
    FORMAT_BC3_UNORM_SRGB = 78,
    FORMAT_BC4_TYPELESS = 79,
    FORMAT_BC4_UNORM = 80,
    FORMAT_BC4_SNORM = 81,
    FORMAT_BC5_TYPELESS = 82,
    FORMAT_BC5_UNORM = 83,
    FORMAT_BC5_SNORM = 84,
    FORMAT_B5G6R5_UNORM = 85,
    FORMAT_B5G5R5A1_UNORM = 86,
    FORMAT_B8G8R8A8_UNORM = 87,
    FORMAT_B8G8R8X8_UNORM = 88,
    FORMAT_R10G10B10_XR_BIAS_A2_UNORM = 89,
    FORMAT_B8G8R8A8_TYPELESS = 90,
    FORMAT_B8G8R8A8_UNORM_SRGB = 91,
    FORMAT_B8G8R8X8_TYPELESS = 92,
    FORMAT_B8G8R8X8_UNORM_SRGB = 93,
    FORMAT_BC6H_TYPELESS = 94,
    FORMAT_BC6H_UF16 = 95,
    FORMAT_BC6H_SF16 = 96,
    FORMAT_BC7_TYPELESS = 97,
    FORMAT_BC7_UNORM = 98,
    FORMAT_BC7_UNORM_SRGB = 99,
    FORMAT_AYUV = 100,
    FORMAT_Y410 = 101,
    FORMAT_Y416 = 102,
    FORMAT_NV12 = 103,
    FORMAT_P010 = 104,
    FORMAT_P016 = 105,
    FORMAT_420_OPAQUE = 106,
    FORMAT_YUY2 = 107,
    FORMAT_Y210 = 108,
    FORMAT_Y216 = 109,
    FORMAT_NV11 = 110,
    FORMAT_AI44 = 111,
    FORMAT_IA44 = 112,
    FORMAT_P8 = 113,
    FORMAT_A8P8 = 114,
    FORMAT_B4G4R4A4_UNORM = 115,

    FORMAT_P208 = 130,
    FORMAT_V208 = 131,
    FORMAT_V408 = 132,


    FORMAT_FORCE_UINT = 0xffffffff
} FORMAT;


typedef
enum RESOURCE_FLAGS
{
    RESOURCE_FLAG_NONE = 0,
    RESOURCE_FLAG_ALLOW_RENDER_TARGET = 0x1,
    RESOURCE_FLAG_ALLOW_DEPTH_STENCIL = 0x2,
    RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS = 0x4,
    RESOURCE_FLAG_DENY_SHADER_RESOURCE = 0x8,
    RESOURCE_FLAG_ALLOW_CROSS_ADAPTER = 0x10,
    RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS = 0x20,
    RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY = 0x40
} 	RESOURCE_FLAGS;
DEFINE_ENUM_FLAG_OPERATORS2(RESOURCE_FLAGS);

typedef
enum RESOURCE_STATES
{
    RESOURCE_STATE_COMMON = 0,
    RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER = 0x1,
    RESOURCE_STATE_INDEX_BUFFER = 0x2,
    RESOURCE_STATE_RENDER_TARGET = 0x4,
    RESOURCE_STATE_UNORDERED_ACCESS = 0x8,
    RESOURCE_STATE_DEPTH_WRITE = 0x10,
    RESOURCE_STATE_DEPTH_READ = 0x20,
    RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE = 0x40,
    RESOURCE_STATE_PIXEL_SHADER_RESOURCE = 0x80,
    RESOURCE_STATE_STREAM_OUT = 0x100,
    RESOURCE_STATE_INDIRECT_ARGUMENT = 0x200,
    RESOURCE_STATE_COPY_DEST = 0x400,
    RESOURCE_STATE_COPY_SOURCE = 0x800,
    RESOURCE_STATE_RESOLVE_DEST = 0x1000,
    RESOURCE_STATE_RESOLVE_SOURCE = 0x2000,
    RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE = 0x400000,
    RESOURCE_STATE_SHADING_RATE_SOURCE = 0x1000000,
    RESOURCE_STATE_GENERIC_READ = (((((0x1 | 0x2) | 0x40) | 0x80) | 0x200) | 0x800),
    RESOURCE_STATE_PRESENT = 0,
    RESOURCE_STATE_PREDICATION = 0x200,
    RESOURCE_STATE_VIDEO_DECODE_READ = 0x10000,
    RESOURCE_STATE_VIDEO_DECODE_WRITE = 0x20000,
    RESOURCE_STATE_VIDEO_PROCESS_READ = 0x40000,
    RESOURCE_STATE_VIDEO_PROCESS_WRITE = 0x80000,
    RESOURCE_STATE_VIDEO_ENCODE_READ = 0x200000,
    RESOURCE_STATE_VIDEO_ENCODE_WRITE = 0x800000
} 	RESOURCE_STATES;
DEFINE_ENUM_FLAG_OPERATORS2(RESOURCE_STATES);

typedef
enum HEAP_TYPE
{
    HEAP_TYPE_DEFAULT = 1,
    HEAP_TYPE_UPLOAD = 2,
    HEAP_TYPE_READBACK = 3,
    HEAP_TYPE_CUSTOM = 4
} 	HEAP_TYPE;

typedef
enum PRIMITIVE_TOPOLOGY
{
    PRIMITIVE_TOPOLOGY_UNDEFINED = 0,
    PRIMITIVE_TOPOLOGY_POINTLIST = 1,
    PRIMITIVE_TOPOLOGY_LINELIST = 2,
    PRIMITIVE_TOPOLOGY_LINESTRIP = 3,
    PRIMITIVE_TOPOLOGY_TRIANGLELIST = 4,
    PRIMITIVE_TOPOLOGY_TRIANGLESTRIP = 5,
    PRIMITIVE_TOPOLOGY_LINELIST_ADJ = 10,
    PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ = 11,
    PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ = 12,
    PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ = 13,
    PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST = 33,
    PRIMITIVE_TOPOLOGY_2_CONTROL_POINT_PATCHLIST = 34,
    PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST = 35,
    PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST = 36,
    PRIMITIVE_TOPOLOGY_5_CONTROL_POINT_PATCHLIST = 37,
    PRIMITIVE_TOPOLOGY_6_CONTROL_POINT_PATCHLIST = 38,
    PRIMITIVE_TOPOLOGY_7_CONTROL_POINT_PATCHLIST = 39,
    PRIMITIVE_TOPOLOGY_8_CONTROL_POINT_PATCHLIST = 40,
    PRIMITIVE_TOPOLOGY_9_CONTROL_POINT_PATCHLIST = 41,
    PRIMITIVE_TOPOLOGY_10_CONTROL_POINT_PATCHLIST = 42,
    PRIMITIVE_TOPOLOGY_11_CONTROL_POINT_PATCHLIST = 43,
    PRIMITIVE_TOPOLOGY_12_CONTROL_POINT_PATCHLIST = 44,
    PRIMITIVE_TOPOLOGY_13_CONTROL_POINT_PATCHLIST = 45,
    PRIMITIVE_TOPOLOGY_14_CONTROL_POINT_PATCHLIST = 46,
    PRIMITIVE_TOPOLOGY_15_CONTROL_POINT_PATCHLIST = 47,
    PRIMITIVE_TOPOLOGY_16_CONTROL_POINT_PATCHLIST = 48,
    PRIMITIVE_TOPOLOGY_17_CONTROL_POINT_PATCHLIST = 49,
    PRIMITIVE_TOPOLOGY_18_CONTROL_POINT_PATCHLIST = 50,
    PRIMITIVE_TOPOLOGY_19_CONTROL_POINT_PATCHLIST = 51,
    PRIMITIVE_TOPOLOGY_20_CONTROL_POINT_PATCHLIST = 52,
    PRIMITIVE_TOPOLOGY_21_CONTROL_POINT_PATCHLIST = 53,
    PRIMITIVE_TOPOLOGY_22_CONTROL_POINT_PATCHLIST = 54,
    PRIMITIVE_TOPOLOGY_23_CONTROL_POINT_PATCHLIST = 55,
    PRIMITIVE_TOPOLOGY_24_CONTROL_POINT_PATCHLIST = 56,
    PRIMITIVE_TOPOLOGY_25_CONTROL_POINT_PATCHLIST = 57,
    PRIMITIVE_TOPOLOGY_26_CONTROL_POINT_PATCHLIST = 58,
    PRIMITIVE_TOPOLOGY_27_CONTROL_POINT_PATCHLIST = 59,
    PRIMITIVE_TOPOLOGY_28_CONTROL_POINT_PATCHLIST = 60,
    PRIMITIVE_TOPOLOGY_29_CONTROL_POINT_PATCHLIST = 61,
    PRIMITIVE_TOPOLOGY_30_CONTROL_POINT_PATCHLIST = 62,
    PRIMITIVE_TOPOLOGY_31_CONTROL_POINT_PATCHLIST = 63,
    PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST = 64,
} 	PRIMITIVE_TOPOLOGY;

typedef
enum CLEAR_FLAGS
{
    CLEAR_FLAG_DEPTH = 0x1,
    CLEAR_FLAG_STENCIL = 0x2
} 	CLEAR_FLAGS;

DEFINE_ENUM_FLAG_OPERATORS2(CLEAR_FLAGS);


/*
* 
*/
typedef
enum PRIMITIVE_TOPOLOGY_TYPE
{
    PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED = 0,
    PRIMITIVE_TOPOLOGY_TYPE_POINT = 1,
    PRIMITIVE_TOPOLOGY_TYPE_LINE = 2,
    PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE = 3,
    PRIMITIVE_TOPOLOGY_TYPE_PATCH = 4
} 	PRIMITIVE_TOPOLOGY_TYPE;

typedef
enum INPUT_CLASSIFICATION
{
    INPUT_CLASSIFICATION_PER_VERTEX_DATA = 0,
    INPUT_CLASSIFICATION_PER_INSTANCE_DATA = 1
} 	INPUT_CLASSIFICATION;

typedef struct INPUT_ELEMENT_DESC
{
    const char* SemanticName;
    UINT SemanticIndex;
    FORMAT Format;
    UINT InputSlot;
    UINT AlignedByteOffset;
    INPUT_CLASSIFICATION InputSlotClass;
    UINT InstanceDataStepRate;
} 	INPUT_ELEMENT_DESC;

typedef struct INPUT_LAYOUT_DESC
{
    INPUT_ELEMENT_DESC* pInputElementDescs;
    UINT NumElements;
    UINT VertexStride;
} 	INPUT_LAYOUT_DESC;



typedef
enum CULL_MODE
{
    CULL_MODE_NONE = 1,
    CULL_MODE_FRONT = 2,
    CULL_MODE_BACK = 3
} 	CULL_MODE;


typedef
enum BLEND
{
    BLEND_ZERO = 1,
    BLEND_ONE = 2,
    BLEND_SRC_COLOR = 3,
    BLEND_INV_SRC_COLOR = 4,
    BLEND_SRC_ALPHA = 5,
    BLEND_INV_SRC_ALPHA = 6,
    BLEND_DEST_ALPHA = 7,
    BLEND_INV_DEST_ALPHA = 8,
    BLEND_DEST_COLOR = 9,
    BLEND_INV_DEST_COLOR = 10,
    BLEND_SRC_ALPHA_SAT = 11,
    BLEND_BLEND_FACTOR = 14,
    BLEND_INV_BLEND_FACTOR = 15,
    BLEND_SRC1_COLOR = 16,
    BLEND_INV_SRC1_COLOR = 17,
    BLEND_SRC1_ALPHA = 18,
    BLEND_INV_SRC1_ALPHA = 19
} 	BLEND;

typedef
enum BLEND_OP
{
   BLEND_OP_ADD = 1,
   BLEND_OP_SUBTRACT = 2,
   BLEND_OP_REV_SUBTRACT = 3,
   BLEND_OP_MIN = 4,
   BLEND_OP_MAX = 5
} 	BLEND_OP;

typedef
enum LOGIC_OP
{
    LOGIC_OP_CLEAR = 0,
    LOGIC_OP_SET = (LOGIC_OP_CLEAR + 1),
    LOGIC_OP_COPY = (LOGIC_OP_SET + 1),
    LOGIC_OP_COPY_INVERTED = (LOGIC_OP_COPY + 1),
    LOGIC_OP_NOOP = (LOGIC_OP_COPY_INVERTED + 1),
    LOGIC_OP_INVERT = (LOGIC_OP_NOOP + 1),
    LOGIC_OP_AND = (LOGIC_OP_INVERT + 1),
    LOGIC_OP_NAND = (LOGIC_OP_AND + 1),
    LOGIC_OP_OR = (LOGIC_OP_NAND + 1),
    LOGIC_OP_NOR = (LOGIC_OP_OR + 1),
    LOGIC_OP_XOR = (LOGIC_OP_NOR + 1),
    LOGIC_OP_EQUIV = (LOGIC_OP_XOR + 1),
    LOGIC_OP_AND_REVERSE = (LOGIC_OP_EQUIV + 1),
    LOGIC_OP_AND_INVERTED = (LOGIC_OP_AND_REVERSE + 1),
    LOGIC_OP_OR_REVERSE = (LOGIC_OP_AND_INVERTED + 1),
    LOGIC_OP_OR_INVERTED = (LOGIC_OP_OR_REVERSE + 1)
} 	LOGIC_OP;

typedef
enum COLOR_WRITE_ENABLE
{
    COLOR_WRITE_ENABLE_RED = 1,
    COLOR_WRITE_ENABLE_GREEN = 2,
    COLOR_WRITE_ENABLE_BLUE = 4,
    COLOR_WRITE_ENABLE_ALPHA = 8,
    COLOR_WRITE_ENABLE_ALL = (((COLOR_WRITE_ENABLE_RED | COLOR_WRITE_ENABLE_GREEN) | COLOR_WRITE_ENABLE_BLUE) | COLOR_WRITE_ENABLE_ALPHA)
} 	COLOR_WRITE_ENABLE;

typedef struct RENDER_TARGET_BLEND_DESC
{
    BOOL BlendEnable;
    BOOL LogicOpEnable;
    BLEND SrcBlend;
    BLEND DestBlend;
    BLEND_OP BlendOp;
    BLEND SrcBlendAlpha;
    BLEND DestBlendAlpha;
    BLEND_OP BlendOpAlpha;
    LOGIC_OP LogicOp;
    UINT8 RenderTargetWriteMask;
} 	RENDER_TARGET_BLEND_DESC;

typedef struct BLEND_DESC
{
    BOOL AlphaToCoverageEnable;
    BOOL IndependentBlendEnable;
    RENDER_TARGET_BLEND_DESC RenderTarget[8];
} 	BLEND_DESC;

#define	DEFAULT_STENCIL_READ_MASK	( 0xff )
#define	DEFAULT_STENCIL_WRITE_MASK	( 0xff )

typedef
enum COMPARISON_FUNC
{
    COMPARISON_FUNC_NEVER = 1,
    COMPARISON_FUNC_LESS = 2,
    COMPARISON_FUNC_EQUAL = 3,
    COMPARISON_FUNC_LESS_EQUAL = 4,
    COMPARISON_FUNC_GREATER = 5,
    COMPARISON_FUNC_NOT_EQUAL = 6,
    COMPARISON_FUNC_GREATER_EQUAL = 7,
    COMPARISON_FUNC_ALWAYS = 8
} 	COMPARISON_FUNC;

typedef
enum DEPTH_WRITE_MASK
{
    DEPTH_WRITE_MASK_ZERO = 0,
    DEPTH_WRITE_MASK_ALL = 1
} 	DEPTH_WRITE_MASK;

typedef
enum STENCIL_OP
{
    STENCIL_OP_KEEP = 1,
    STENCIL_OP_ZERO = 2,
    STENCIL_OP_REPLACE = 3,
    STENCIL_OP_INCR_SAT = 4,
    STENCIL_OP_DECR_SAT = 5,
    STENCIL_OP_INVERT = 6,
    STENCIL_OP_INCR = 7,
    STENCIL_OP_DECR = 8
} 	STENCIL_OP;

typedef struct DEPTH_STENCILOP_DESC
{
    STENCIL_OP StencilFailOp;
    STENCIL_OP StencilDepthFailOp;
    STENCIL_OP StencilPassOp;
    COMPARISON_FUNC StencilFunc;
} 	DEPTH_STENCILOP_DESC;

typedef struct DEPTH_STENCIL_DESC
{
    BOOL DepthEnable;
    DEPTH_WRITE_MASK DepthWriteMask;
    COMPARISON_FUNC DepthFunc;
    BOOL StencilEnable;
    UINT8 StencilReadMask;
    UINT8 StencilWriteMask;
    DEPTH_STENCILOP_DESC FrontFace;
    DEPTH_STENCILOP_DESC BackFace;
} 	DEPTH_STENCIL_DESC;

struct ShaderDefine
{
    std::wstring Name;
    std::wstring Value;
};

struct SHADER_CREATE_DESC
{
    std::wstring Dir;
    std::wstring FileName;
    std::wstring EntryPoint;
    std::wstring Target;
    std::optional<std::vector<ShaderDefine>> Defines;
};

typedef struct GRAPHICS_PIPELINE_STATE_DESC
{
    BLEND_DESC BlendState;
    UINT SampleMask;
    //RASTERIZER_DESC RasterizerState;
    CULL_MODE CullMode;
    DEPTH_STENCIL_DESC DepthStencilState;
    INPUT_LAYOUT_DESC InputLayout;
    PRIMITIVE_TOPOLOGY_TYPE PrimitiveTopologyType = PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED; // only dx12 use this.
    PRIMITIVE_TOPOLOGY PrimitiveTopology = PRIMITIVE_TOPOLOGY_UNDEFINED; // only vulkan use this.
    UINT NumRenderTargets;
    FORMAT RTVFormats[8];
    FORMAT DSVFormat;
    UINT MultiSampleCount;
    UINT MultiSampleQuality;
    UINT NodeMask;

    SHADER_CREATE_DESC* vsDesc = nullptr;
    SHADER_CREATE_DESC* psDesc = nullptr;


} 	GRAPHICS_PIPELINE_STATE_DESC;


typedef
enum PIPELINE_STATE_FLAGS
{
    PIPELINE_STATE_FLAG_NONE = 0,
    PIPELINE_STATE_FLAG_TOOL_DEBUG = 0x1
} 	PIPELINE_STATE_FLAGS;

DEFINE_ENUM_FLAG_OPERATORS2(PIPELINE_STATE_FLAGS);

typedef struct COMPUTE_PIPELINE_STATE_DESC
{
    SHADER_CREATE_DESC* csDesc = nullptr;
    UINT NodeMask;
    PIPELINE_STATE_FLAGS Flags;
} 	COMPUTE_PIPELINE_STATE_DESC;

struct SAMPLER_DESC
{
    FILTER Filter;
    TEXTURE_ADDRESS_MODE AddressU;
    TEXTURE_ADDRESS_MODE AddressV;
    TEXTURE_ADDRESS_MODE AddressW;
    float MipLODBias;
    unsigned int MaxAnisotropy;
    COMPARISON_FUNC ComparisonFunc;
    float BorderColor[4];
    float MinLOD;
    float MaxLOD;
};

class GfxSampler
{
public:
    GfxSampler(){}
    virtual ~GfxSampler(){}
};

class GfxPipelineStateObject
{
public:
    GfxPipelineStateObject() {}
    virtual ~GfxPipelineStateObject() {}
};

struct RTPSO_DESC
{
    unsigned int MaxRecursion = 1;
    unsigned int MaxAttributeSizeInBytes;
    unsigned int MaxPayloadSizeInBytes;

    std::wstring Dir;
    std::wstring ShaderFile;
    std::optional<std::vector<ShaderDefine>> Defines;
};

enum RTShaderType
{
    GLOBAL,
    RAYGEN,
    MISS,
    HIT,
    ANYHIT
};

class GfxRTPipelineStateObject
{
public:
    GfxRTPipelineStateObject() {}
    virtual ~GfxRTPipelineStateObject() {}
};


class GfxCommandList
{
public:
    GfxCommandList(){}
    virtual ~GfxCommandList() {}
};

class GfxTexture
{
public:
    GfxTexture() {}
    virtual ~GfxTexture() {}
};

class GfxBuffer
{
public:
    GfxBuffer() {}
    virtual ~GfxBuffer() {}
};

class GfxIndexBuffer
{
public:
    GfxIndexBuffer() {}
    virtual ~GfxIndexBuffer() {}
};

class GfxVertexBuffer
{
public:
    GfxVertexBuffer() {}
    virtual ~GfxVertexBuffer() {}
};

class GfxMesh;
class GfxRTAS
{
public:
    GfxMesh* mesh;
    GfxRTAS() {}
    virtual ~GfxRTAS(){}
};

class GfxDescriptor
{

};

struct Rect
{
    long    left;
    long    top;
    long    right;
    long    bottom;
};

struct ViewPort
{
    float TopLeftX;
    float TopLeftY;
    float Width;
    float Height;
    float MinDepth;
    float MaxDepth;
};

struct ResourceTransition
{
    union Res
    {
    GfxTexture * texture;
    GfxBuffer* buffer;
    };
    Res res;
    RESOURCE_STATES before;
    RESOURCE_STATES after;
    
    enum ResType
    {
        TEXTURE,
        BUFFER
    };
    ResType resType = TEXTURE;
    ResourceTransition(GfxTexture* intexture, RESOURCE_STATES inbefore, RESOURCE_STATES inafter)
    {
        resType = TEXTURE;
        res.texture = intexture;
        before = inbefore;
        after = inafter;
    }
    ResourceTransition(GfxBuffer* inbuffer, RESOURCE_STATES inbefore, RESOURCE_STATES inafter)
    {
        resType = BUFFER;
        res.buffer = inbuffer;
        before = inbefore;
        after = inafter;
    }
};

struct SUBRESOURCE_DATA
{
    const void* pData;
    __int64 RowPitch;
    __int64 SlicePitch;
};


class GfxMaterial
{
public:
    bool bHasAlpha = false;

    std::shared_ptr<GfxTexture> Diffuse;
    std::shared_ptr<GfxTexture> Normal;
    std::shared_ptr<GfxTexture> Roughness;
    std::shared_ptr<GfxTexture> Metallic;

    GfxMaterial() {}
    ~GfxMaterial(){}
};


class GfxMesh
{
public:
    struct DrawCall
    {
        std::shared_ptr<GfxMaterial> mat;
        INT DiffuseTextureIndex;
        INT NormalTextureIndex;
        INT SpecularTextureIndex;
        UINT IndexStart;
        UINT IndexCount;
        UINT VertexBase;
        UINT VertexCount;
    };
public:
    bool bTransparent = false;
    glm::mat4x4 transform;
    UINT NumIndices;
    UINT NumVertices;

    UINT VertexStride;

    FORMAT IndexFormat = FORMAT_R32_UINT;

    std::shared_ptr<GfxIndexBuffer> Ib;
    std::shared_ptr<GfxVertexBuffer> Vb;
    //std::vector<std::shared_ptr<GfxTexture>> Textures;

    std::shared_ptr<GfxMaterial> Mat;

    std::vector<DrawCall> Draws;
};

class Scene
{
public:
    glm::vec3 AABBMin = glm::vec3(0, 0, 0);
    glm::vec3 AABBMax = glm::vec3(0, 0, 0);
    float BoundingRadius = 0.f;

    void SetTransform(glm::mat4x4 inTransform);

public:
    std::vector<std::shared_ptr<GfxMesh>> meshes;
    std::vector<std::shared_ptr<GfxMaterial>> Materials;
public:
};

class AbstractGfxLayer
{
public:
	static GfxSampler* CreateSampler(SAMPLER_DESC& InSamplerDesc);
    static void SetSampler(std::string bindName, GfxCommandList* cl, GfxPipelineStateObject* PSO, GfxSampler* sampler);

    static GfxTexture* CreateTextureFromFile(std::wstring fileName, bool nonSRGB);
    static GfxTexture* CreateTexture2D(FORMAT format, RESOURCE_FLAGS resFlags, RESOURCE_STATES initResState, int width, int height, int mipLevels, std::optional<glm::vec4> clearColor = std::nullopt);
    static GfxTexture* CreateTexture3D(FORMAT format, RESOURCE_FLAGS resFlags, RESOURCE_STATES initResState, int width, int height, int depth, int mipLevels);

    static GfxVertexBuffer* CreateVertexBuffer(UINT Size, UINT Stride, void* SrcData);
    static GfxIndexBuffer* CreateIndexBuffer(FORMAT Format, UINT Size, void* SrcData);
    static GfxBuffer* CreateByteAddressBuffer(UINT InNumElements, UINT InElementSize, HEAP_TYPE InType, RESOURCE_STATES initResState, RESOURCE_FLAGS InFlags, void* SrcData = nullptr);

    static GfxRTAS* CreateTLAS(std::vector<std::shared_ptr<GfxRTAS>>& VecBLAS);
    static GfxRTAS* CreateBLAS(GfxMesh* mesh);

    static void MapBuffer(GfxBuffer* buffer, void** pData);
    static void UnmapBuffer(GfxBuffer* buffer);


    static GfxPipelineStateObject* CreatePSO();
    static GfxRTPipelineStateObject* CreateRTPSO();

    static bool InitPSO(GfxPipelineStateObject* PSO, GRAPHICS_PIPELINE_STATE_DESC* psoDesc);
    static bool InitPSO(GfxPipelineStateObject* PSO, COMPUTE_PIPELINE_STATE_DESC* psoDesc);


    static void SetReadTexture(GfxPipelineStateObject* PSO, std::string name, GfxTexture* texture, GfxCommandList* CL);
    static void SetWriteTexture(GfxPipelineStateObject* PSO, std::string name, GfxTexture* texture, GfxCommandList* CL);

    static void SetReadBuffer(GfxPipelineStateObject* PSO, std::string name, GfxBuffer* buffer, GfxCommandList* CL);
    static void SetWriteBuffer(GfxPipelineStateObject* PSO, std::string name, GfxBuffer* buffer, GfxCommandList* CL);

    static void SetUniformValue(GfxPipelineStateObject* PSO, std::string name, void* pData, GfxCommandList* CL);
    static void SetUniformBuffer(GfxPipelineStateObject* PSO, std::string name, GfxBuffer* buffer, int offset, GfxCommandList* CL);

    static void SetPSO(GfxPipelineStateObject* PSO, GfxCommandList* CL);


    static void DrawInstanced(GfxCommandList* CL, int VertexCountPerInstance, int InstanceCount, int StartVertexLocation, int StartInstanceLocation) ;
    static void DrawIndexedInstanced(GfxCommandList* CL, int IndexCountPerInstance, int InstanceCount, int StartIndexLocation, int BaseVertexLocation, int StartInstanceLocation);

    static void SetVertexBuffer(GfxCommandList* CL, int StartSlot, int NumViews, GfxVertexBuffer* buffer);
    static void SetIndexBuffer(GfxCommandList* CL, GfxIndexBuffer* buffer);

    static void SetPrimitiveTopology(GfxCommandList* CL, PRIMITIVE_TOPOLOGY PrimitiveTopology);

    static void SetScissorRects(GfxCommandList* CL, int NumRects, Rect* rect);
    static void SetViewports(GfxCommandList* CL, int NumViewPorts, ViewPort* rect);

    static void ClearRenderTarget(GfxCommandList* CL, GfxTexture* texture, float ColorRGBA[4], int NumRects, Rect* rects);
    static void ClearDepthStencil(GfxCommandList* CL, GfxTexture* texture, CLEAR_FLAGS ClearFlags, float Depth, unsigned char Stencil, int NumRects, Rect* rects);

    static void SetRenderTargets(GfxCommandList* CL, GfxPipelineStateObject* PSO, int NumRendertargets, GfxTexture** Rendertargets, GfxTexture* DepthTexture);

    static void Dispatch(GfxCommandList* CL, int ThreadGroupCountX, int ThreadGroupCountY, int ThreadGroupCountZ);

    // normal pso
    static void BindSRV(GfxPipelineStateObject* PSO, std::string name, int baseRegister, int num);
    static void BindSampler(GfxPipelineStateObject* PSO, std::string name, int baseRegister);
    static void BindCBV(GfxPipelineStateObject* PSO, std::string name, int baseRegister, int size);
    static void BindUAV(GfxPipelineStateObject* PSO, std::string name, int baseRegister);

    // rt pso
    static void AddHitGroup(GfxRTPipelineStateObject* PSO, std::string name, std::string chs, std::string ahs);
    static void AddShader(GfxRTPipelineStateObject* PSO, std::string shader, RTShaderType shaderType);
    static void BindUAV(GfxRTPipelineStateObject* PSO, std::string shader, std::string name, int baseRegister);
    static void BindSRV(GfxRTPipelineStateObject* PSO, std::string shader, std::string name, int baseRegister);
    static void BindSampler(GfxRTPipelineStateObject* PSO, std::string shader, std::string name, int baseRegister);
    static void BindCBV(GfxRTPipelineStateObject* PSO, std::string shader, std::string name, int baseRegister, int size);

    static void SetUAV(GfxRTPipelineStateObject* PSO, std::string shader, std::string bindingName, GfxTexture* texture, int instanceIndex = -1);
    static void SetSRV(GfxRTPipelineStateObject* PSO, std::string shader, std::string bindingName, GfxTexture* texture, int instanceIndex = -1);
    static void SetSRV(GfxRTPipelineStateObject* PSO, std::string shader, std::string bindingName, GfxRTAS* rtas, int instanceIndex = -1);
    static void SetSampler(GfxRTPipelineStateObject* PSO, std::string shader, std::string bindingName, GfxSampler* sampler, int instanceIndex = -1);
    static void SetCBVValue(GfxRTPipelineStateObject* PSO, std::string shader, std::string bindingName, void* pData, int instanceIndex = -1);
    static void SetCBVValue(GfxRTPipelineStateObject* PSO, std::string shader, std::string bindingName, unsigned __int64 GPUAddr, int instanceIndex = -1);

    static void BeginShaderTable(GfxRTPipelineStateObject* PSO);
    static void EndShaderTable(GfxRTPipelineStateObject* PSO, UINT NumInstance);
    static void ResetHitProgram(GfxRTPipelineStateObject* PSO, int instanceIndex);
    static void StartHitProgram(GfxRTPipelineStateObject* PSO, std::string HitGroup, int instanceIndex);
    static void AddDescriptor2HitProgram(GfxRTPipelineStateObject* PSO, std::string HitGroup, GfxDescriptor* des, int instanceIndex);
    static void AddSRVDescriptor2HitProgram(GfxRTPipelineStateObject* PSO, std::string HitGroup, GfxTexture* resource, int instanceIndex);
    static void AddSRVDescriptor2HitProgram(GfxRTPipelineStateObject* PSO, std::string HitGroup, GfxVertexBuffer* resource, int instanceIndex);
    static void AddSRVDescriptor2HitProgram(GfxRTPipelineStateObject* PSO, std::string HitGroup, GfxIndexBuffer* resource, int instanceIndex);
    static void AddSRVDescriptor2HitProgram(GfxRTPipelineStateObject* PSO, std::string HitGroup, GfxBuffer* resource, int instanceIndex);


    static GfxDescriptor* GetSRV(GfxTexture* texture);
    static GfxDescriptor* GetSRV(GfxBuffer* buffer);
    static GfxDescriptor* GetSRV(GfxVertexBuffer* buffer);
    static GfxDescriptor* GetSRV(GfxIndexBuffer* buffer);

    static void UploadSRCData3D(GfxTexture* texture, SUBRESOURCE_DATA* SrcData);



    static void DispatchRay(GfxRTPipelineStateObject* PSO, int width, int height, GfxCommandList* CL, int NumInstance);

    static bool InitRTPSO(GfxRTPipelineStateObject* PSO, RTPSO_DESC* desc);

    static void TransitionResource(GfxCommandList* CL, int NumTransition, ResourceTransition* transitions);

    static void GetFrameBuffers(std::vector<std::shared_ptr<GfxTexture>>& FrameFuffers);
    
    static void BeginFrame(std::list<GfxTexture*>& DynamicTexture);
    static void EndFrame();
    static void OnSizeChanged(std::vector<std::shared_ptr<GfxTexture>>& FrameFuffers, int width, int height, bool minimized);
    static void NameTexture(GfxTexture* texture, std::wstring name);
    static void NameBuffer(GfxBuffer* texture, std::wstring name);


    static void ExecuteCommandList(GfxCommandList* cmd);

    static void SetDescriptorHeap(GfxCommandList* CL);

    static GfxCommandList* GetGlobalCommandList();

    static int GetCurrentFrameIndex();

    static void WaitGPUFlush();

    static bool IsDX12();

    static void CreateDX12API(HWND hWnd, UINT DisplayWidth, UINT DisplayHeight);

    static void CreateVulkanAPI(HINSTANCE hInstance, HWND hWnd, UINT DisplayWidth, UINT DisplayHeight);

    static void Release();
};

#define NAME_TEXTURE(x) AbstractGfxLayer::NameTexture(x.get(), L#x);
#define NAME_BUFFER(x) AbstractGfxLayer::NameBuffer(x.get(), L#x);



class AbstractGfxLayerScopeGPUProfile
{
public:

    template<typename... ARGS>
    AbstractGfxLayerScopeGPUProfile(GfxCommandList* cl, UINT64 color, PCSTR formatString, ARGS... args)
    {
        if (AbstractGfxLayer::IsDX12())
        {
            CommandList* dx12CL = static_cast<CommandList*>(cl);
            PIXBeginEvent(dx12CL->CmdList.Get(), color, formatString, args...);
        }
    }

    ~AbstractGfxLayerScopeGPUProfile()
    {
        if (AbstractGfxLayer::IsDX12())
            PIXEndEvent();
    }
};

#define ProfileGPUScope(...) AbstractGfxLayerScopeGPUProfile p(__VA_ARGS__);
