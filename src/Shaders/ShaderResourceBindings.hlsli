#ifndef SHADER_RESOURCE_BINDINGS_HLSLI
#define SHADER_RESOURCE_BINDINGS_HLSLI

#define VULKAN_TEXTURE_BINDING(regIndex) (regIndex)
#define VULKAN_SAMPLER_BINDING(regIndex) (64 + (regIndex))
#define VULKAN_CBUFFER_BINDING(regIndex) (128 + (regIndex))

#if defined(VULKAN_SPIRV)
#define TEXTURE2D_BINDING(name, regIndex) [[vk::binding(VULKAN_TEXTURE_BINDING(regIndex), 0)]] Texture2D name
#define SAMPLER_BINDING(name, regIndex) [[vk::binding(VULKAN_SAMPLER_BINDING(regIndex), 0)]] SamplerState name
#define CBUFFER_BINDING_BEGIN(name, regIndex) [[vk::binding(VULKAN_CBUFFER_BINDING(regIndex), 0)]] cbuffer name
#else
#define TEXTURE2D_BINDING(name, regIndex) Texture2D name : register(t##regIndex)
#define SAMPLER_BINDING(name, regIndex) SamplerState name : register(s##regIndex)
#define CBUFFER_BINDING_BEGIN(name, regIndex) cbuffer name : register(b##regIndex)
#endif

#define CBUFFER_BINDING_END

#endif
