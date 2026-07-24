#pragma once

#include <cstdint>

namespace CoronaMeshFormat
{
    inline constexpr char Magic[8] = { 'C', 'R', 'N', 'M', 'E', 'S', 'H', '\0' };
    inline constexpr std::uint32_t Version = 2;
    inline constexpr std::uint32_t MaterialHasAlpha = 1u << 0;
    inline constexpr std::uint32_t MaxStringBytes = 64u * 1024u;
    inline constexpr std::uint32_t MaxMaterials = 4096u;
    inline constexpr std::uint32_t MaxMeshes = 65536u;

    struct FileHeader
    {
        char Magic[8]{};
        std::uint32_t Version = 0;
        std::uint32_t HeaderSize = 0;
        std::uint32_t MaterialCount = 0;
        std::uint32_t MeshCount = 0;
        std::uint32_t Flags = 0;
        float BoundsMin[3]{};
        float BoundsMax[3]{};
    };

    struct MeshHeader
    {
        std::uint32_t MaterialIndex = 0;
        std::uint32_t VertexCount = 0;
        std::uint32_t IndexCount = 0;
        std::uint32_t Reserved = 0;
    };

    struct Vertex
    {
        float Position[3]{};
        float Normal[3]{};
        float UV[2]{};
        float Tangent[3]{};
    };

    static_assert(sizeof(FileHeader) == 52, "Unexpected .cmesh file-header layout.");
    static_assert(sizeof(MeshHeader) == 16, "Unexpected .cmesh mesh-header layout.");
    static_assert(sizeof(Vertex) == 44, "Unexpected .cmesh vertex layout.");
}
