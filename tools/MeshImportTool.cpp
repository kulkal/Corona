#define NOMINMAX
#include <Windows.h>

#include "CoronaMeshFormat.h"
#include "ufbx.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    struct Vec2
    {
        float X = 0.0f;
        float Y = 0.0f;
    };

    struct Vec3
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
    };

    Vec3 operator+(const Vec3& a, const Vec3& b) { return { a.X + b.X, a.Y + b.Y, a.Z + b.Z }; }
    Vec3 operator-(const Vec3& a, const Vec3& b) { return { a.X - b.X, a.Y - b.Y, a.Z - b.Z }; }
    Vec3 operator*(const Vec3& v, float scale) { return { v.X * scale, v.Y * scale, v.Z * scale }; }
    Vec3& operator+=(Vec3& a, const Vec3& b) { a = a + b; return a; }

    float Dot(const Vec3& a, const Vec3& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
    Vec3 Cross(const Vec3& a, const Vec3& b)
    {
        return {
            a.Y * b.Z - a.Z * b.Y,
            a.Z * b.X - a.X * b.Z,
            a.X * b.Y - a.Y * b.X,
        };
    }

    float LengthSquared(const Vec3& v) { return Dot(v, v); }

    Vec3 NormalizeOr(const Vec3& v, const Vec3& fallback)
    {
        const float lengthSquared = LengthSquared(v);
        if (!(lengthSquared > 1.0e-20f) || !std::isfinite(lengthSquared))
            return fallback;
        return v * (1.0f / std::sqrt(lengthSquared));
    }

    Vec3 OrthogonalTangent(const Vec3& normal)
    {
        const Vec3 axis = std::abs(normal.Z) < 0.999f ? Vec3{ 0.0f, 0.0f, 1.0f } : Vec3{ 0.0f, 1.0f, 0.0f };
        return NormalizeOr(Cross(axis, normal), { 1.0f, 0.0f, 0.0f });
    }

    std::uint32_t FloatBits(float value)
    {
        if (value == 0.0f)
            value = 0.0f;
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    struct PositionKey
    {
        std::uint32_t X = 0;
        std::uint32_t Y = 0;
        std::uint32_t Z = 0;

        bool operator==(const PositionKey& other) const
        {
            return X == other.X && Y == other.Y && Z == other.Z;
        }
    };

    struct PositionKeyHash
    {
        std::size_t operator()(const PositionKey& key) const
        {
            std::size_t hash = key.X;
            hash ^= static_cast<std::size_t>(key.Y) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
            hash ^= static_cast<std::size_t>(key.Z) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
            return hash;
        }
    };

    PositionKey MakePositionKey(const Vec3& value)
    {
        return { FloatBits(value.X), FloatBits(value.Y), FloatBits(value.Z) };
    }

    struct VertexKey
    {
        std::array<std::uint32_t, 8> Bits{};

        bool operator<(const VertexKey& other) const { return Bits < other.Bits; }
    };

    struct Corner
    {
        Vec3 Position;
        Vec3 FaceNormal;
        Vec3 Normal;
        Vec2 UV;
    };

    struct Triangle
    {
        std::uint32_t Corner[3]{};
        std::uint32_t MaterialIndex = 0;
    };

    struct MaterialRecord
    {
        std::uint32_t Flags = 0;
        std::string Diffuse;
        std::string Normal;
        std::string Roughness;
        std::string Metallic;
    };

    struct MeshRecord
    {
        std::uint32_t MaterialIndex = 0;
        std::vector<CoronaMeshFormat::Vertex> Vertices;
        std::vector<std::uint32_t> Indices;
    };

    struct MeshBuilder
    {
        std::map<VertexKey, std::uint32_t> VertexLookup;
        std::vector<CoronaMeshFormat::Vertex> Vertices;
        std::vector<std::uint32_t> Indices;
        std::vector<Vec3> TangentSums;
    };

    struct Arguments
    {
        fs::path Input;
        fs::path Output;
        float NormalAngleDegrees = 45.0f;
        bool Force = false;
        bool Quiet = false;
    };

    std::wstring Usage()
    {
        return
            L"CoronaMeshImport --input <model.fbx|model.obj> [--output <model.cmesh>]\n"
            L"                  [--normal-angle <degrees>] [--force] [--quiet]\n";
    }

    Arguments ParseArguments(int argc, wchar_t** argv)
    {
        Arguments result;
        for (int index = 1; index < argc; ++index)
        {
            const std::wstring argument = argv[index];
            auto requireValue = [&]() -> const wchar_t*
            {
                if (index + 1 >= argc)
                    throw std::runtime_error("Missing command-line option value.");
                return argv[++index];
            };

            if (argument == L"--input" || argument == L"-i")
                result.Input = requireValue();
            else if (argument == L"--output" || argument == L"-o")
                result.Output = requireValue();
            else if (argument == L"--normal-angle")
                result.NormalAngleDegrees = std::stof(requireValue());
            else if (argument == L"--force")
                result.Force = true;
            else if (argument == L"--quiet")
                result.Quiet = true;
            else if (argument == L"--help" || argument == L"-h" || argument == L"/?")
            {
                std::wcout << Usage();
                std::exit(0);
            }
            else
                throw std::runtime_error("Unknown command-line option.");
        }

        if (result.Input.empty())
            throw std::runtime_error("--input is required.");
        if (result.Output.empty())
        {
            result.Output = result.Input;
            result.Output.replace_extension(L".cmesh");
        }
        if (!std::isfinite(result.NormalAngleDegrees) || result.NormalAngleDegrees < 0.0f || result.NormalAngleDegrees > 180.0f)
            throw std::runtime_error("--normal-angle must be between 0 and 180 degrees.");
        return result;
    }

    std::wstring ToLower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch)
        {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return value;
    }

    std::string UfbxString(ufbx_string value)
    {
        return value.data && value.length > 0 ? std::string(value.data, value.length) : std::string();
    }

    std::string FileName(std::string value)
    {
        std::replace(value.begin(), value.end(), '\\', '/');
        const std::size_t slash = value.find_last_of('/');
        return slash == std::string::npos ? value : value.substr(slash + 1);
    }

    std::string TextureName(const ufbx_material_map& map)
    {
        const ufbx_texture* texture = map.texture;
        if (!texture)
            return {};
        if (texture->file_textures.count > 0)
            texture = texture->file_textures.data[0];

        std::string value = UfbxString(texture->relative_filename);
        if (value.empty())
            value = UfbxString(texture->filename);
        return FileName(std::move(value));
    }

    std::string PreferTexture(const ufbx_material_map& primary, const ufbx_material_map& fallback)
    {
        std::string value = TextureName(primary);
        return value.empty() ? TextureName(fallback) : value;
    }

    const std::map<std::string, std::string>& SponzaRoughnessMap()
    {
        static const std::map<std::string, std::string> values = {
            { "Background_Albedo", "Background_Roughness" },
            { "ChainTexture_Albedo", "ChainTexture_Roughness" },
            { "Lion_Albedo", "Lion_Roughness" },
            { "Sponza_Arch_diffuse", "Sponza_Arch_roughness" },
            { "Sponza_Bricks_a_Albedo", "Sponza_Bricks_a_Roughness" },
            { "Sponza_Ceiling_diffuse", "Sponza_Ceiling_roughness" },
            { "Sponza_Column_a_diffuse", "Sponza_Column_a_roughness" },
            { "Sponza_Column_b_diffuse", "Sponza_Column_b_roughness" },
            { "Sponza_Column_c_diffuse", "Sponza_Column_c_roughness" },
            { "Sponza_Curtain_Blue_diffuse", "Sponza_Curtain_roughness" },
            { "Sponza_Curtain_Green_diffuse", "Sponza_Curtain_roughness" },
            { "Sponza_Curtain_Red_diffuse", "Sponza_Curtain_roughness" },
            { "Sponza_Details_diffuse", "Sponza_Details_roughness" },
            { "Sponza_Fabric_Blue_diffuse", "Sponza_Fabric_roughness" },
            { "Sponza_Fabric_Green_diffuse", "Sponza_Fabric_roughness" },
            { "Sponza_Fabric_Red_diffuse", "Sponza_Fabric_roughness" },
            { "Sponza_FlagPole_diffuse", "Sponza_FlagPole_roughness" },
            { "Sponza_Floor_diffuse", "Sponza_Floor_roughness" },
            { "Sponza_Roof_diffuse", "Sponza_Roof_roughness" },
            { "Sponza_Thorn_diffuse", "Sponza_Thorn_roughness" },
            { "Vase_diffuse", "Vase_roughness" },
            { "VaseHanging_diffuse", "Vase_roughness" },
            { "VasePlant_diffuse", "Vase_roughness" },
            { "VaseRound_diffuse", "Vase_roughness" },
        };
        return values;
    }

    bool IsAlphaDiffuse(const std::string& value)
    {
        std::string lower = value;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch)
        {
            return static_cast<char>(std::tolower(ch));
        });
        return lower == "sponza_thorn_diffuse.png" ||
            lower == "vaseplant_diffuse.png" ||
            lower == "chaintexture_albedo.png";
    }

    std::vector<fs::path> MaterialSidecarCandidates(const fs::path& modelPath)
    {
        std::vector<fs::path> result;
        result.emplace_back(modelPath.wstring() + L".materials.json");
        fs::path extensionReplaced = modelPath;
        extensionReplaced.replace_extension(L".materials.json");
        if (extensionReplaced != result.front())
            result.push_back(std::move(extensionReplaced));
        return result;
    }

    std::vector<MaterialRecord> LoadMaterialSidecar(const fs::path& modelPath)
    {
        for (const fs::path& candidate : MaterialSidecarCandidates(modelPath))
        {
            std::ifstream file(candidate, std::ios::binary);
            if (!file)
                continue;

            nlohmann::json root;
            file >> root;
            const auto materialIt = root.find("materials");
            if (materialIt == root.end() || !materialIt->is_array())
                throw std::runtime_error("Material sidecar does not contain a materials array.");

            std::vector<MaterialRecord> result;
            for (const nlohmann::json& value : *materialIt)
            {
                MaterialRecord material;
                material.Diffuse = FileName(value.value("diffuse", ""));
                material.Normal = FileName(value.value("normal", ""));
                material.Roughness = FileName(value.value("roughness", ""));
                material.Metallic = FileName(value.value("metallic", ""));
                material.Flags = value.value("flags", 0u);
                if (IsAlphaDiffuse(material.Diffuse))
                    material.Flags |= CoronaMeshFormat::MaterialHasAlpha;
                result.push_back(std::move(material));
            }
            return result;
        }
        return {};
    }

    std::vector<MaterialRecord> BuildMaterials(const ufbx_scene* scene, const fs::path& modelPath)
    {
        std::vector<MaterialRecord> result;
        result.reserve(std::max<std::size_t>(scene->materials.count, 1));
        for (const ufbx_material* material : scene->materials)
        {
            MaterialRecord record;
            record.Diffuse = PreferTexture(material->pbr.base_color, material->fbx.diffuse_color);
            record.Normal = TextureName(material->pbr.normal_map);
            if (record.Normal.empty())
                record.Normal = TextureName(material->fbx.normal_map);
            if (record.Normal.empty())
                record.Normal = TextureName(material->fbx.bump);
            record.Roughness = TextureName(material->pbr.roughness);
            record.Metallic = TextureName(material->pbr.metalness);
            if (record.Metallic.empty())
                record.Metallic = TextureName(material->fbx.ambient_color);

            if (record.Roughness.empty() && !record.Diffuse.empty())
            {
                const std::string stem = fs::u8path(record.Diffuse).stem().u8string();
                const auto it = SponzaRoughnessMap().find(stem);
                if (it != SponzaRoughnessMap().end())
                    record.Roughness = it->second + ".png";
            }
            if (IsAlphaDiffuse(record.Diffuse))
                record.Flags |= CoronaMeshFormat::MaterialHasAlpha;
            result.push_back(std::move(record));
        }
        if (result.empty())
            result.emplace_back();

        const std::vector<MaterialRecord> sidecar = LoadMaterialSidecar(modelPath);
        for (std::size_t index = 0; index < sidecar.size(); ++index)
        {
            if (index >= result.size())
                result.resize(index + 1);
            result[index] = sidecar[index];
        }
        if (result.size() > CoronaMeshFormat::MaxMaterials)
            throw std::runtime_error("Material count exceeds the .cmesh format limit.");
        return result;
    }

    bool IsUpToDate(const Arguments& arguments)
    {
        if (arguments.Force)
            return false;
        std::error_code error;
        if (!fs::is_regular_file(arguments.Output, error))
            return false;
        const fs::file_time_type outputTime = fs::last_write_time(arguments.Output, error);
        if (error || outputTime < fs::last_write_time(arguments.Input, error) || error)
            return false;
        for (const fs::path& sidecar : MaterialSidecarCandidates(arguments.Input))
        {
            if (fs::is_regular_file(sidecar, error) && !error && outputTime < fs::last_write_time(sidecar, error))
                return false;
            error.clear();
        }
        return true;
    }

    Vec3 ToVec3(ufbx_vec3 value)
    {
        return { static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z) };
    }

    std::uint32_t ResolveMaterialIndex(const ufbx_node* node, const ufbx_mesh* mesh, std::uint32_t localIndex, std::size_t materialCount)
    {
        const ufbx_material* material = nullptr;
        if (localIndex < node->materials.count)
            material = node->materials.data[localIndex];
        else if (localIndex < mesh->materials.count)
            material = mesh->materials.data[localIndex];
        if (!material || material->typed_id >= materialCount)
            return 0;
        return material->typed_id;
    }

    void GenerateSmoothNormals(std::vector<Corner>& corners, float angleDegrees)
    {
        std::unordered_map<PositionKey, std::vector<std::uint32_t>, PositionKeyHash> positionCorners;
        positionCorners.reserve(corners.size());
        for (std::uint32_t index = 0; index < corners.size(); ++index)
            positionCorners[MakePositionKey(corners[index].Position)].push_back(index);

        const float radians = angleDegrees * 3.14159265358979323846f / 180.0f;
        const float cosineThreshold = std::cos(radians);
        for (const auto& entry : positionCorners)
        {
            const std::vector<std::uint32_t>& group = entry.second;
            for (std::uint32_t cornerIndex : group)
            {
                const Vec3 reference = NormalizeOr(corners[cornerIndex].FaceNormal, { 0.0f, 1.0f, 0.0f });
                Vec3 sum{};
                for (std::uint32_t candidateIndex : group)
                {
                    const Vec3 candidate = NormalizeOr(corners[candidateIndex].FaceNormal, reference);
                    if (Dot(reference, candidate) + 1.0e-6f >= cosineThreshold)
                        sum += corners[candidateIndex].FaceNormal;
                }
                corners[cornerIndex].Normal = NormalizeOr(sum, reference);
            }
        }
    }

    VertexKey MakeVertexKey(const Corner& corner)
    {
        return { {
            FloatBits(corner.Position.X), FloatBits(corner.Position.Y), FloatBits(corner.Position.Z),
            FloatBits(corner.Normal.X), FloatBits(corner.Normal.Y), FloatBits(corner.Normal.Z),
            FloatBits(corner.UV.X), FloatBits(corner.UV.Y),
        } };
    }

    std::uint32_t AddVertex(MeshBuilder& builder, const Corner& corner)
    {
        const VertexKey key = MakeVertexKey(corner);
        const auto existing = builder.VertexLookup.find(key);
        if (existing != builder.VertexLookup.end())
            return existing->second;
        if (builder.Vertices.size() >= std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("Vertex count exceeds the .cmesh format limit.");

        CoronaMeshFormat::Vertex vertex{};
        vertex.Position[0] = corner.Position.X;
        vertex.Position[1] = corner.Position.Y;
        vertex.Position[2] = corner.Position.Z;
        vertex.Normal[0] = corner.Normal.X;
        vertex.Normal[1] = corner.Normal.Y;
        vertex.Normal[2] = corner.Normal.Z;
        vertex.UV[0] = corner.UV.X;
        vertex.UV[1] = corner.UV.Y;

        const std::uint32_t index = static_cast<std::uint32_t>(builder.Vertices.size());
        builder.VertexLookup.emplace(key, index);
        builder.Vertices.push_back(vertex);
        builder.TangentSums.emplace_back();
        return index;
    }

    std::vector<MeshRecord> BuildMeshes(
        const ufbx_scene* scene,
        std::size_t materialCount,
        float normalAngleDegrees,
        Vec3& boundsMin,
        Vec3& boundsMax)
    {
        std::map<std::uint32_t, MeshBuilder> builders;
        bool hasBounds = false;

        for (const ufbx_node* node : scene->nodes)
        {
            const ufbx_mesh* mesh = node->mesh;
            if (!mesh || mesh->num_triangles == 0 || !mesh->vertex_position.exists)
                continue;

            std::vector<Corner> corners;
            std::vector<Triangle> triangles;
            corners.reserve(mesh->num_triangles * 3);
            triangles.reserve(mesh->num_triangles);
            std::vector<std::uint32_t> triangulated(std::max<std::size_t>(mesh->max_face_triangles * 3, 3));

            for (std::size_t faceIndex = 0; faceIndex < mesh->faces.count; ++faceIndex)
            {
                if (mesh->face_hole.count > faceIndex && mesh->face_hole.data[faceIndex])
                    continue;
                const ufbx_face face = mesh->faces.data[faceIndex];
                if (face.num_indices < 3)
                    continue;
                const std::uint32_t triangleCount = ufbx_triangulate_face(
                    triangulated.data(), triangulated.size(), mesh, face);
                const std::uint32_t localMaterial = mesh->face_material.count > faceIndex
                    ? mesh->face_material.data[faceIndex]
                    : 0;
                const std::uint32_t materialIndex = ResolveMaterialIndex(node, mesh, localMaterial, materialCount);

                for (std::uint32_t triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex)
                {
                    Triangle triangle{};
                    triangle.MaterialIndex = materialIndex;
                    for (std::uint32_t vertex = 0; vertex < 3; ++vertex)
                    {
                        const std::uint32_t meshIndex = triangulated[triangleIndex * 3 + vertex];
                        const ufbx_vec3 localPosition = ufbx_get_vertex_vec3(&mesh->vertex_position, meshIndex);
                        const ufbx_vec3 worldPosition = ufbx_transform_position(&node->geometry_to_world, localPosition);
                        Corner corner{};
                        corner.Position = ToVec3(worldPosition);
                        if (mesh->vertex_uv.exists)
                        {
                            const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, meshIndex);
                            corner.UV = { static_cast<float>(uv.x), 1.0f - static_cast<float>(uv.y) };
                        }
                        triangle.Corner[vertex] = static_cast<std::uint32_t>(corners.size());
                        corners.push_back(corner);

                        if (!hasBounds)
                        {
                            boundsMin = boundsMax = corner.Position;
                            hasBounds = true;
                        }
                        else
                        {
                            boundsMin.X = std::min(boundsMin.X, corner.Position.X);
                            boundsMin.Y = std::min(boundsMin.Y, corner.Position.Y);
                            boundsMin.Z = std::min(boundsMin.Z, corner.Position.Z);
                            boundsMax.X = std::max(boundsMax.X, corner.Position.X);
                            boundsMax.Y = std::max(boundsMax.Y, corner.Position.Y);
                            boundsMax.Z = std::max(boundsMax.Z, corner.Position.Z);
                        }
                    }

                    const Vec3 edgeA = corners[triangle.Corner[1]].Position - corners[triangle.Corner[0]].Position;
                    const Vec3 edgeB = corners[triangle.Corner[2]].Position - corners[triangle.Corner[0]].Position;
                    const Vec3 faceNormal = Cross(edgeA, edgeB);
                    if (LengthSquared(faceNormal) <= 1.0e-20f)
                    {
                        corners.resize(corners.size() - 3);
                        continue;
                    }
                    for (std::uint32_t vertex = 0; vertex < 3; ++vertex)
                        corners[triangle.Corner[vertex]].FaceNormal = faceNormal;
                    triangles.push_back(triangle);
                }
            }

            GenerateSmoothNormals(corners, normalAngleDegrees);
            for (const Triangle& triangle : triangles)
            {
                MeshBuilder& builder = builders[triangle.MaterialIndex];
                std::uint32_t indices[3]{};
                for (std::uint32_t vertex = 0; vertex < 3; ++vertex)
                {
                    indices[vertex] = AddVertex(builder, corners[triangle.Corner[vertex]]);
                    builder.Indices.push_back(indices[vertex]);
                }

                const Corner& a = corners[triangle.Corner[0]];
                const Corner& b = corners[triangle.Corner[1]];
                const Corner& c = corners[triangle.Corner[2]];
                const Vec3 edgeA = b.Position - a.Position;
                const Vec3 edgeB = c.Position - a.Position;
                const float deltaU1 = b.UV.X - a.UV.X;
                const float deltaV1 = b.UV.Y - a.UV.Y;
                const float deltaU2 = c.UV.X - a.UV.X;
                const float deltaV2 = c.UV.Y - a.UV.Y;
                const float determinant = deltaU1 * deltaV2 - deltaV1 * deltaU2;
                Vec3 tangent{};
                if (std::abs(determinant) > 1.0e-20f)
                    tangent = (edgeA * deltaV2 - edgeB * deltaV1) * (1.0f / determinant);
                for (std::uint32_t index : indices)
                    builder.TangentSums[index] += tangent;
            }
        }

        if (!hasBounds || builders.empty())
            throw std::runtime_error("The source file contains no renderable triangles.");
        if (builders.size() > CoronaMeshFormat::MaxMeshes)
            throw std::runtime_error("Mesh count exceeds the .cmesh format limit.");

        std::vector<MeshRecord> result;
        result.reserve(builders.size());
        for (auto& entry : builders)
        {
            MeshBuilder& builder = entry.second;
            for (std::size_t index = 0; index < builder.Vertices.size(); ++index)
            {
                CoronaMeshFormat::Vertex& vertex = builder.Vertices[index];
                const Vec3 normal = { vertex.Normal[0], vertex.Normal[1], vertex.Normal[2] };
                Vec3 tangent = builder.TangentSums[index] - normal * Dot(normal, builder.TangentSums[index]);
                tangent = NormalizeOr(tangent, OrthogonalTangent(normal));
                vertex.Tangent[0] = tangent.X;
                vertex.Tangent[1] = tangent.Y;
                vertex.Tangent[2] = tangent.Z;
            }

            MeshRecord mesh;
            mesh.MaterialIndex = entry.first;
            mesh.Vertices = std::move(builder.Vertices);
            mesh.Indices = std::move(builder.Indices);
            result.push_back(std::move(mesh));
        }
        return result;
    }

    template <typename T>
    void WriteValue(std::ofstream& file, const T& value)
    {
        file.write(reinterpret_cast<const char*>(&value), sizeof(value));
        if (!file)
            throw std::runtime_error("Failed while writing the .cmesh file.");
    }

    void WriteString(std::ofstream& file, const std::string& value)
    {
        if (value.size() > CoronaMeshFormat::MaxStringBytes)
            throw std::runtime_error("Material texture path exceeds the .cmesh format limit.");
        const std::uint32_t length = static_cast<std::uint32_t>(value.size());
        WriteValue(file, length);
        if (length > 0)
            file.write(value.data(), length);
        if (!file)
            throw std::runtime_error("Failed while writing a material texture path.");
    }

    void WriteMeshFile(
        const fs::path& outputPath,
        const std::vector<MaterialRecord>& materials,
        const std::vector<MeshRecord>& meshes,
        const Vec3& boundsMin,
        const Vec3& boundsMax)
    {
        if (!outputPath.parent_path().empty())
            fs::create_directories(outputPath.parent_path());
        fs::path temporaryPath = outputPath;
        temporaryPath += L".tmp." + std::to_wstring(GetCurrentProcessId());

        try
        {
            std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
            if (!file)
                throw std::runtime_error("Could not open the temporary .cmesh output file.");

            CoronaMeshFormat::FileHeader header{};
            std::memcpy(header.Magic, CoronaMeshFormat::Magic, sizeof(header.Magic));
            header.Version = CoronaMeshFormat::Version;
            header.HeaderSize = sizeof(header);
            header.MaterialCount = static_cast<std::uint32_t>(materials.size());
            header.MeshCount = static_cast<std::uint32_t>(meshes.size());
            header.BoundsMin[0] = boundsMin.X;
            header.BoundsMin[1] = boundsMin.Y;
            header.BoundsMin[2] = boundsMin.Z;
            header.BoundsMax[0] = boundsMax.X;
            header.BoundsMax[1] = boundsMax.Y;
            header.BoundsMax[2] = boundsMax.Z;
            WriteValue(file, header);

            for (const MaterialRecord& material : materials)
            {
                WriteValue(file, material.Flags);
                WriteString(file, material.Diffuse);
                WriteString(file, material.Normal);
                WriteString(file, material.Roughness);
                WriteString(file, material.Metallic);
            }
            for (const MeshRecord& mesh : meshes)
            {
                if (mesh.Vertices.size() > std::numeric_limits<std::uint32_t>::max() ||
                    mesh.Indices.size() > std::numeric_limits<std::uint32_t>::max())
                    throw std::runtime_error("Mesh data exceeds the .cmesh format limit.");
                CoronaMeshFormat::MeshHeader meshHeader{};
                meshHeader.MaterialIndex = mesh.MaterialIndex;
                meshHeader.VertexCount = static_cast<std::uint32_t>(mesh.Vertices.size());
                meshHeader.IndexCount = static_cast<std::uint32_t>(mesh.Indices.size());
                WriteValue(file, meshHeader);
                file.write(reinterpret_cast<const char*>(mesh.Vertices.data()),
                    static_cast<std::streamsize>(mesh.Vertices.size() * sizeof(mesh.Vertices[0])));
                file.write(reinterpret_cast<const char*>(mesh.Indices.data()),
                    static_cast<std::streamsize>(mesh.Indices.size() * sizeof(mesh.Indices[0])));
                if (!file)
                    throw std::runtime_error("Failed while writing mesh data.");
            }
            file.flush();
            if (!file)
                throw std::runtime_error("Failed while flushing the .cmesh file.");
            file.close();

            if (!MoveFileExW(temporaryPath.c_str(), outputPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Could not atomically replace the .cmesh output file.");
        }
        catch (...)
        {
            std::error_code error;
            fs::remove(temporaryPath, error);
            throw;
        }
    }

    int Import(const Arguments& arguments)
    {
        const auto start = std::chrono::steady_clock::now();
        std::error_code error;
        if (!fs::is_regular_file(arguments.Input, error))
            throw std::runtime_error("Input model does not exist or is not a regular file.");
        const std::wstring extension = ToLower(arguments.Input.extension().wstring());
        if (extension != L".fbx" && extension != L".obj")
            throw std::runtime_error("Only FBX and OBJ source models are supported.");
        if (IsUpToDate(arguments))
        {
            if (!arguments.Quiet)
                std::wcout << L"up to date: " << arguments.Output << L"\n";
            return 0;
        }

        ufbx_load_opts options{};
        options.ignore_animation = true;
        options.ignore_embedded = true;
        options.load_external_files = extension == L".obj";
        options.ignore_missing_external_files = true;
        options.generate_missing_normals = true;
        options.normalize_normals = true;
        options.normalize_tangents = true;
        options.use_blender_pbr_material = true;
        options.target_axes = ufbx_axes_left_handed_y_up;
        options.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
        options.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;

        const std::string inputUtf8 = arguments.Input.u8string();
        ufbx_error loadError{};
        ufbx_scene* scene = ufbx_load_file(inputUtf8.c_str(), &options, &loadError);
        if (!scene)
        {
            const std::string description = UfbxString(loadError.description);
            throw std::runtime_error("ufbx load failed: " + description);
        }

        try
        {
            const std::vector<MaterialRecord> materials = BuildMaterials(scene, arguments.Input);
            Vec3 boundsMin{};
            Vec3 boundsMax{};
            const std::vector<MeshRecord> meshes = BuildMeshes(
                scene, materials.size(), arguments.NormalAngleDegrees, boundsMin, boundsMax);
            ufbx_free_scene(scene);
            scene = nullptr;

            WriteMeshFile(arguments.Output, materials, meshes, boundsMin, boundsMax);
            if (!arguments.Quiet)
            {
                std::uint64_t vertexCount = 0;
                std::uint64_t triangleCount = 0;
                for (const MeshRecord& mesh : meshes)
                {
                    vertexCount += mesh.Vertices.size();
                    triangleCount += mesh.Indices.size() / 3;
                }
                const double elapsedMilliseconds = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count();
                std::wcout
                    << L"wrote " << arguments.Output
                    << L" | meshes=" << meshes.size()
                    << L" materials=" << materials.size()
                    << L" vertices=" << vertexCount
                    << L" triangles=" << triangleCount
                    << L" ufbx=" << ufbx_version_major(UFBX_VERSION) << L'.'
                    << ufbx_version_minor(UFBX_VERSION) << L'.' << ufbx_version_patch(UFBX_VERSION)
                    << L" elapsed=" << elapsedMilliseconds << L" ms\n";
            }
        }
        catch (...)
        {
            if (scene)
                ufbx_free_scene(scene);
            throw;
        }
        return 0;
    }
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        return Import(ParseArguments(argc, argv));
    }
    catch (const std::exception& exception)
    {
        std::cerr << "CoronaMeshImport: " << exception.what() << '\n';
        std::wcerr << Usage();
        return 1;
    }
}
