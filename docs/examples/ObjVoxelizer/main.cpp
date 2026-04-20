// ProjectV OBJ Voxelizer
// A command-line tool that converts Wavefront OBJ models into ProjectV's chunked voxel format.
// It parses mesh geometry and material data, samples diffuse textures at each voxel's surface
// intersection, and writes the result as a collection of spatially organized voxel chunks.
//
// ProjectV Engine Features Used:
//   - Core Math       : vec2/vec3/ivec3, dot, cross, min/max/clamp — used for intersection math and UV sampling
//   - Logging         : info/warn/error via spdlog wrapper for structured, leveled output
//   - Voxel I/O       : writeChunkToDisk for serializing voxel chunks to the ProjectV scene format
//   - Voxel Mgmt      : ChunkHeader, VoxelBatch, createChunk, moveVoxelBatchToChunk, updateChunkFromItsVoxelBatch
//   - Z-Order Indexing: createZOrderIndex / reverseZOrderIndex for Morton-coded spatial layout within chunks
//   - ECS             : core ECS types included as the foundation for all ProjectV scene objects

#include "core/ecs.h"
#include "core/math.h"
#include "core/log.h"
#include "utils/voxel_io.h"
#include "utils/voxel_management.h"

#define TINYOBJLOADER_IMPLEMENTATION
#include "include/tiny_obj_loader.h"

#define STB_IMAGE_IMPLEMENTATION
#include "include/stb_image.h"

#include "include/CLI.hpp"

#include <algorithm>
#include <iostream>
#include <string>
using namespace projv::core;

struct Vertex {
    vec3 position;
    vec3 normal;
    vec2 texCoord;
    std::string texture;
    vec3 materialColor{200.0f, 200.0f, 200.0f}; // diffuse fallback (0-255 range)
};

struct Triangle {
    vec3 v0, v1, v2;
};

struct AABB {
    vec3 min;
    vec3 max;
};

bool triAABBIntersect(const Triangle& tri, const AABB& box) {
    // Compute box center and half extents
    vec3 c = (box.min + box.max) * 0.5f;
    vec3 e = (box.max - box.min) * 0.5f;

    vec3 triangleMax = max(max(tri.v0, tri.v1), tri.v2);
    vec3 triangleMin = min(min(tri.v0, tri.v1), tri.v2);

    // Early AABB vs AABB rejection
    if (triangleMax.x < box.min.x || triangleMin.x > box.max.x ||
        triangleMax.y < box.min.y || triangleMin.y > box.max.y ||
        triangleMax.z < box.min.z || triangleMin.z > box.max.z) {
        return false; // No overlap → no intersection
    }

    // Move triangle to box-centered space
    vec3 v0 = tri.v0 - c;
    vec3 v1 = tri.v1 - c;
    vec3 v2 = tri.v2 - c;

    // Triangle edges
    vec3 f0 = v1 - v0;
    vec3 f1 = v2 - v1;
    vec3 f2 = v0 - v2;

    // === 1. Cross product axes (9 tests) ===
    vec3 axes[9] = {
        cross(vec3(1,0,0), f0), cross(vec3(1,0,0), f1), cross(vec3(1,0,0), f2),
        cross(vec3(0,1,0), f0), cross(vec3(0,1,0), f1), cross(vec3(0,1,0), f2),
        cross(vec3(0,0,1), f0), cross(vec3(0,0,1), f1), cross(vec3(0,0,1), f2)
    };

    for (int i = 0; i < 9; ++i) {
        vec3 axis = axes[i];

        // Skip near-zero axis (parallel edges)
        if (length(axis) < 1e-8f) continue;

        float p0 = dot(v0, axis);
        float p1 = dot(v1, axis);
        float p2 = dot(v2, axis);

        float r = e.x * abs(axis.x) + e.y * abs(axis.y) + e.z * abs(axis.z);

        float minP = std::min({p0, p1, p2});
        float maxP = std::max({p0, p1, p2});

        if (minP > r || maxP < -r)
            return false;
    }

    // === 2. AABB face axes ===
    if (std::min({v0.x, v1.x, v2.x}) > e.x || std::max({v0.x, v1.x, v2.x}) < -e.x) return false;
    if (std::min({v0.y, v1.y, v2.y}) > e.y || std::max({v0.y, v1.y, v2.y}) < -e.y) return false;
    if (std::min({v0.z, v1.z, v2.z}) > e.z || std::max({v0.z, v1.z, v2.z}) < -e.z) return false;

    // === 3. Triangle normal ===
    vec3 normal = cross(f0, f1);

    float d = -dot(normal, v0);

    vec3 vmin, vmax;
    for (int i = 0; i < 3; ++i) {
        if (normal[i] > 0.0f) {
            vmin[i] = -e[i];
            vmax[i] =  e[i];
        } else {
            vmin[i] =  e[i];
            vmax[i] = -e[i];
        }
    }

    if (dot(normal, vmin) + d > 0.0f) return false;
    if (dot(normal, vmax) + d < 0.0f) return false;

    return true;
}

struct Texture {
    unsigned char *image;
    ivec2 size;
    uint channels;
};

void voxelizeObjFile(std::filesystem::path modelObjDirectory, std::string modelMtlDirectory, std::string textureDirectory, int voxelizationResolution, std::string outputDirectory) {
    using namespace projv::core;
    info("--------------------------------------------");
    info("  ProjectV Voxelizer");
    info("  Input:      {}", modelObjDirectory.string());
    info("  Output:     {}", outputDirectory);
    info("  Resolution: {}", voxelizationResolution);
    info("--------------------------------------------");

    info("Loading OBJ model...");
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    tinyobj::attrib_t attributes;
    std::string warnings;
    std::string errors;
    bool success = tinyobj::LoadObj(&attributes, &shapes, &materials, &warnings, &errors, modelObjDirectory.c_str(), modelMtlDirectory.c_str(), true);

    if (!success) {
        projv::core::error("Failed to load OBJ: {}", errors);
        return;
    }
    if (!warnings.empty()) {
        projv::core::warn("OBJ loader warnings: {}", warnings);
    }
    info("  {} shape(s), {} material(s) found.", shapes.size(), materials.size());

    // Pre-load all textures referenced by materials
    std::unordered_map<std::string, Texture> textures;
    std::filesystem::path textureDirPath = textureDirectory;
    int texturesLoaded = 0, texturesFailed = 0;
    if (!materials.empty()) info("Loading textures...");
    for (const auto& mat : materials) {
        std::string texName = mat.diffuse_texname;
        if (texName.empty() || textures.count(texName) > 0) continue;

        // Normalize path separators (handle Windows-style backslashes in .mtl files)
        std::string normalizedName = texName;
        std::replace(normalizedName.begin(), normalizedName.end(), '\\', '/');

        std::filesystem::path texPath = textureDirPath / normalizedName;
        int width, height, channels;
        Texture texture;
        texture.image = stbi_load(texPath.c_str(), &width, &height, &channels, 4);
        if (texture.image == nullptr) {
            projv::core::warn("  [MISSING] '{}' - {}", normalizedName, stbi_failure_reason());
            texture.size = {0, 0};
            texture.channels = 0;
            texturesFailed++;
        } else {
            texture.size = {width, height};
            texture.channels = 4;
            projv::core::info("  [OK] '{}' ({}x{})", normalizedName, width, height);
            texturesLoaded++;
        }
        textures[texName] = texture; // store even on failure so we don't retry
    }

    info("Parsing geometry...");
    std::vector<Vertex> vertices;

    // IMPORTANT: initialize AABB correctly
    vec3 verticesMin = { FLT_MAX, FLT_MAX, FLT_MAX };
    vec3 verticesMax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

    for (const auto& shape : shapes) {
        const auto& mesh = shape.mesh;

        size_t indexOffset = 0;

        for (size_t f = 0; f < mesh.num_face_vertices.size(); f++) {
            int fv = mesh.num_face_vertices[f];
            int mat_id = mesh.material_ids[f];

            std::string tex = "";
            projv::core::vec3 matDiffuse = {200.0f, 200.0f, 200.0f};
            if (mat_id >= 0 && mat_id < (int)materials.size()) {
                tex = materials[mat_id].diffuse_texname;
                matDiffuse = {
                    materials[mat_id].diffuse[0] * 255.0f,
                    materials[mat_id].diffuse[1] * 255.0f,
                    materials[mat_id].diffuse[2] * 255.0f
                };
            }

            if (fv < 3) {
                indexOffset += fv; // skip degenerate faces
                continue;
            }

            auto getVertex = [&](const tinyobj::index_t& index) -> Vertex {
                projv::core::vec3 position = {
                    attributes.vertices[3 * index.vertex_index + 0],
                    attributes.vertices[3 * index.vertex_index + 1],
                    attributes.vertices[3 * index.vertex_index + 2]
                };
                position *= vec3(0.01);

                projv::core::vec3 normal = {0, 0, 0};
                if (index.normal_index >= 0) {
                    normal = {
                        attributes.normals[3 * index.normal_index + 0],
                        attributes.normals[3 * index.normal_index + 1],
                        attributes.normals[3 * index.normal_index + 2]
                    };
                }

                projv::core::vec2 texCoord = {0, 0};
                if (index.texcoord_index >= 0) {
                    texCoord = {
                        attributes.texcoords[2 * index.texcoord_index + 0],
                        attributes.texcoords[2 * index.texcoord_index + 1]
                    };
                }

                return {position, normal, texCoord};
            };

            // Triangulate faces with more than 3 vertices (fan triangulation)
            for (int v = 1; v < fv - 1; v++) {
                const tinyobj::index_t& i0 = mesh.indices[indexOffset + 0];
                const tinyobj::index_t& i1 = mesh.indices[indexOffset + v];
                const tinyobj::index_t& i2 = mesh.indices[indexOffset + v + 1];

                Vertex vert0 = getVertex(i0);
                Vertex vert1 = getVertex(i1);
                Vertex vert2 = getVertex(i2);

                verticesMin = projv::core::min(verticesMin, vert0.position);
                verticesMin = projv::core::min(verticesMin, vert1.position);
                verticesMin = projv::core::min(verticesMin, vert2.position);
                verticesMax = projv::core::max(verticesMax, vert0.position);
                verticesMax = projv::core::max(verticesMax, vert1.position);
                verticesMax = projv::core::max(verticesMax, vert2.position);

                vert0.texture = tex;  vert0.materialColor = matDiffuse;
                vert1.texture = tex;  vert1.materialColor = matDiffuse;
                vert2.texture = tex;  vert2.materialColor = matDiffuse;

                vertices.push_back(vert0);
                vertices.push_back(vert1);
                vertices.push_back(vert2);
            }

            indexOffset += fv;
        }
    }

    size_t triangleCount = vertices.size() / 3;
    if (triangleCount == 0) {
        projv::core::error("No triangles found in model. Aborting.");
        return;
    }
    info("  {} triangle(s) parsed.", triangleCount);

    vec3 modelExtents = verticesMax - verticesMin;
    float largestDistanceTotal = max(max(modelExtents.x, modelExtents.y), modelExtents.z);
    info("  Bounding box:  ({:.3f}, {:.3f}, {:.3f})  ->  ({:.3f}, {:.3f}, {:.3f})",
        verticesMin.x, verticesMin.y, verticesMin.z,
        verticesMax.x, verticesMax.y, verticesMax.z);
    info("  Extents (W/H/D): {:.3f} x {:.3f} x {:.3f}  (largest axis: {:.3f})",
        modelExtents.x, modelExtents.y, modelExtents.z, largestDistanceTotal);

    for (size_t vertexIndex = 0; vertexIndex < vertices.size(); vertexIndex++) {
        vertices[vertexIndex].position -= verticesMin;
    }

    uint numberOfChunksPerAxis = std::ceil(voxelizationResolution / 256.0f);
    float chunkScale = largestDistanceTotal / numberOfChunksPerAxis;
    float voxelScale = chunkScale / 256.0f;
    uint totalChunks = numberOfChunksPerAxis * numberOfChunksPerAxis * numberOfChunksPerAxis;

    info("Voxelizing: {} chunk(s) ({} per axis), voxel size: {:.5f}", totalChunks, numberOfChunksPerAxis, voxelScale);

    size_t totalVoxels = 0;
    uint logStep = std::max(1u, totalChunks / 10);

    for (int chunkIndex = 0; chunkIndex < (int)totalChunks; chunkIndex++) {
        if (totalChunks == 1 || chunkIndex % logStep == 0) {
            info("  Chunk {}/{} ({:.0f}%)", chunkIndex + 1, totalChunks,
                100.0f * chunkIndex / totalChunks);
        }
        projv::core::ivec3 chunkIndexPosition = projv::utils::reverseZOrderIndex(chunkIndex);
        projv::ChunkHeader chunkHeader;
        chunkHeader.chunkID = chunkIndex;
        chunkHeader.position = vec3(chunkIndexPosition) * vec3(chunkScale);
        chunkHeader.voxelScale = voxelScale;
        chunkHeader.resolution = 256;
        projv::VoxelBatch voxelBatch;

        projv::Voxel cornerVoxel;
        cornerVoxel.ZOrderPosition = 4;
        cornerVoxel.color = {0, 0, 0};
        voxelBatch.emplace_back(cornerVoxel);

        for (size_t triangleIndex = 0; triangleIndex < vertices.size() / 3; triangleIndex++) {
            const vec3& p0 = vertices[triangleIndex * 3].position;
            const vec3& p1 = vertices[triangleIndex * 3 + 1].position;
            const vec3& p2 = vertices[triangleIndex * 3 + 2].position;
            Triangle tri{p0, p1, p2};

            vec2 texCoordsV0 = vertices[triangleIndex * 3].texCoord;
            vec2 texCoordsV1 = vertices[triangleIndex * 3 + 1].texCoord;
            vec2 texCoordsV2 = vertices[triangleIndex * 3 + 2].texCoord;
            const std::string& textureName = vertices[triangleIndex * 3].texture;
            const vec3& fallbackColor = vertices[triangleIndex * 3].materialColor;

            bool textureExists = false;
            Texture texture;
            if (!textureName.empty()) {
                auto it = textures.find(textureName);
                if (it != textures.end() && it->second.image != nullptr) {
                    texture = it->second;
                    textureExists = true;
                }
            }

            // Precompute barycentric denominator for this triangle
            vec3 edge0 = p1 - p0;
            vec3 edge1 = p2 - p0;
            float d00 = dot(edge0, edge0);
            float d01 = dot(edge0, edge1);
            float d11 = dot(edge1, edge1);
            float baryDenom = d00 * d11 - d01 * d01;
            bool degenerate = (std::abs(baryDenom) < 1e-10f);

            AABB chunkAABB;
            chunkAABB.min = chunkHeader.position;
            chunkAABB.max = chunkHeader.position + vec3(chunkScale);

            vec3 triMin = min(min(tri.v0, tri.v1), tri.v2);
            vec3 triMax = max(max(tri.v0, tri.v1), tri.v2);

            if (triMax.x < chunkAABB.min.x || triMin.x > chunkAABB.max.x ||
                triMax.y < chunkAABB.min.y || triMin.y > chunkAABB.max.y ||
                triMax.z < chunkAABB.min.z || triMin.z > chunkAABB.max.z) {
                continue;
            }

            ivec3 minVoxel = ivec3(floor((triMin - chunkHeader.position) / voxelScale));
            ivec3 maxVoxel = ivec3(ceil((triMax - chunkHeader.position) / voxelScale));
            minVoxel = clamp(minVoxel, ivec3(0), ivec3(255));
            maxVoxel = clamp(maxVoxel, ivec3(0), ivec3(255));

            for (int z = minVoxel.z; z <= maxVoxel.z; z++) {
                for (int y = minVoxel.y; y <= maxVoxel.y; y++) {
                    for (int x = minVoxel.x; x <= maxVoxel.x; x++) {
                        vec3 voxelMin = chunkHeader.position + vec3(x, y, z) * voxelScale;
                        AABB voxelAABB;
                        voxelAABB.min = voxelMin;
                        voxelAABB.max = voxelMin + vec3(voxelScale);

                        if (!triAABBIntersect(tri, voxelAABB)) continue;

                        uint r, g, b;

                        if (textureExists && !degenerate) {
                            // Interpolate UV at voxel center (not triangle centroid)
                            vec3 voxelCenter = (voxelAABB.min + voxelAABB.max) * 0.5f;
                            vec3 v0p = voxelCenter - p0;
                            float d20 = dot(v0p, edge0);
                            float d21 = dot(v0p, edge1);

                            float bary1 = (d11 * d20 - d01 * d21) / baryDenom;
                            float bary2 = (d00 * d21 - d01 * d20) / baryDenom;
                            float bary0 = 1.0f - bary1 - bary2;

                            // Clamp to valid barycentric range and renormalize
                            bary0 = std::max(0.0f, bary0);
                            bary1 = std::max(0.0f, bary1);
                            bary2 = std::max(0.0f, bary2);
                            float barySum = bary0 + bary1 + bary2;
                            if (barySum > 1e-6f) {
                                bary0 /= barySum; bary1 /= barySum; bary2 /= barySum;
                            }

                            vec2 uv = bary0 * texCoordsV0 + bary1 * texCoordsV1 + bary2 * texCoordsV2;
                            uv.y = 1.0f - uv.y; // flip V for OpenGL convention

                            // Wrap UV (fract-equivalent, handles tiling and values outside [0,1])
                            uv.x -= std::floor(uv.x);
                            uv.y -= std::floor(uv.y);

                            int tx = clamp(int(uv.x * texture.size.x), 0, texture.size.x - 1);
                            int ty = clamp(int(uv.y * texture.size.y), 0, texture.size.y - 1);
                            int idx = (ty * texture.size.x + tx) * texture.channels;

                            r = texture.image[idx + 0];
                            g = texture.image[idx + 1];
                            b = texture.image[idx + 2];
                        } else {
                            // Fall back to material diffuse color
                            r = uint(clamp(fallbackColor.x, 0.0f, 255.0f));
                            g = uint(clamp(fallbackColor.y, 0.0f, 255.0f));
                            b = uint(clamp(fallbackColor.z, 0.0f, 255.0f));
                        }

                        projv::Voxel voxel;
                        voxel.ZOrderPosition = projv::utils::createZOrderIndex(ivec3(x, y, z));
                        voxel.color = {uint8_t(r), uint8_t(g), uint8_t(b)};
                        projv::utils::addVoxelToVoxelBatch(voxel, voxelBatch);
                    }
                }
            }
        }

        // Count voxels before batch is consumed (subtract the sentinel corner voxel)
        size_t chunkVoxels = voxelBatch.size() > 0 ? voxelBatch.size() - 1 : 0;
        totalVoxels += chunkVoxels;

        projv::Chunk chunk = projv::utils::createChunk(chunkHeader);
        projv::utils::moveVoxelBatchToChunk(voxelBatch, chunk);
        projv::utils::updateChunkFromItsVoxelBatch(chunk);
        projv::utils::writeChunkToDisk(outputDirectory, chunk);
    }

    // Free all loaded texture data
    for (auto& [name, tex] : textures) {
        if (tex.image) {
            stbi_image_free(tex.image);
            tex.image = nullptr;
        }
    }

    info("--------------------------------------------");
    info("  Voxelization Summary");
    info("  Input file:      {}", modelObjDirectory.string());
    info("  Shapes / Mats:   {} / {}", shapes.size(), materials.size());
    info("  Textures:        {} loaded, {} missing", texturesLoaded, texturesFailed);
    info("  Triangles:       {}", triangleCount);
    info("  Bounding box:    ({:.3f}, {:.3f}, {:.3f})  ->  ({:.3f}, {:.3f}, {:.3f})",
        verticesMin.x, verticesMin.y, verticesMin.z,
        verticesMin.x + modelExtents.x, verticesMin.y + modelExtents.y, verticesMin.z + modelExtents.z);
    info("  Extents (W/H/D): {:.3f} x {:.3f} x {:.3f}", modelExtents.x, modelExtents.y, modelExtents.z);
    info("  Resolution:      {}", voxelizationResolution);
    info("  Chunks:          {} ({} per axis)", totalChunks, numberOfChunksPerAxis);
    info("  Voxel size:      {:.5f} world units", voxelScale);
    info("  Total voxels:    {}", totalVoxels);
    info("  Output:          {}", outputDirectory);
    info("--------------------------------------------");
    if (texturesFailed > 0)
        projv::core::warn("{} texture(s) could not be loaded — affected surfaces used material color fallback.", texturesFailed);
}

int main(int argc, char** argv) {
    CLI::App app{"A voxelizer to convert Wavefront OBJ models to voxel ProjectV models."};
    argv = app.ensure_utf8(argv);

    std::string objModelDirectory = "";
    std::string objFileDirectory = "";
    std::string outputDirectory = "";
    std::string resolutionString = "256";

    app.add_option("-m, --modelDir", objModelDirectory, "Path to root directory of the obj model.");
    app.add_option("-f, --objDir", objFileDirectory, "Path to obj file of the obj model.");
    app.add_option("-o, --outputDir", outputDirectory, "Path to put the generated scene.");
    app.add_option("-r, --resolution", resolutionString, "Resolution to voxelize the scene at.");
    CLI11_PARSE(app, argc, argv);

    int resolution = std::stoi(resolutionString, 0, 10);
    std::filesystem::path objFileDirectoryAsPath = objFileDirectory;

    voxelizeObjFile(objFileDirectoryAsPath, objModelDirectory, objModelDirectory, resolution, outputDirectory);

    return 0;
}
