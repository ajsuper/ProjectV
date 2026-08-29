#ifndef PROJECTV_MESH_IMPORT_HPP
#define PROJECTV_MESH_IMPORT_HPP

// Format-agnostic mesh import for the voxelizer.
//
// The voxelizer only ever wants four things out of a model file: a flat triangle soup in world
// space, a UV per vertex, a diffuse color per triangle, and the image that color is sampled from.
// Every format expresses those differently — a scene graph of instanced nodes in FBX/glTF, a flat
// vertex list in OBJ/STL, textures on disk in OBJ/DAE versus embedded in the container in GLB and
// binary FBX — so this header collapses all of it into one `ImportedModel` and lets the voxelizer
// stay format-blind.
//
// Assimp does the parsing (40+ formats: obj, fbx, gltf/glb, dae, stl, ply, 3ds, blend, x, …), and
// its post-process passes do the normalization that would otherwise land here: triangulating
// n-gons, baking the node hierarchy into world space, and generating UVs where a format allows a
// mapping to be implied rather than stated.
//
// Texture *decoding* is deliberately left to the caller, which owns stb_image: this header hands
// back either a resolved path on disk or the embedded bytes, and the caller turns those into
// pixels. That keeps the one place that allocates and frees image memory in the caller.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <assimp/Importer.hpp>
#include <assimp/importerdesc.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include "core/log.h"
#include "core/math.h"

namespace meshimport {

struct ImportedVertex {
    projv::core::vec3 position;
    projv::core::vec2 texCoord;
};

/**
 * A diffuse texture the model references. Formats carry these two different ways: OBJ, DAE and
 * friends name a file on disk, while GLB and binary FBX routinely embed the image bytes in the
 * container itself. Exactly one of `path`, `encodedBytes` or `rawPixels` is populated.
 */
struct ImportedTexture {
    std::string name;                   // Original reference, for logging.
    std::filesystem::path path;         // Resolved file on disk; empty when embedded.
    std::vector<uint8_t> encodedBytes;  // Embedded and still compressed (png/jpg/…).
    std::vector<uint8_t> rawPixels;     // Embedded and already decoded, RGBA8.
    int rawWidth = 0;
    int rawHeight = 0;
};

struct ImportedMaterial {
    std::string name;
    int textureIndex = -1;                                  // Into ImportedModel::textures; -1 = untextured.
    int heightTextureIndex = -1;                            // Grayscale height/bump map; -1 = none.
    projv::core::vec3 diffuseColor{200.0f, 200.0f, 200.0f}; // 0-255, the untextured fallback.
};

struct ImportedModel {
    std::vector<ImportedVertex> vertices;   // Three per triangle, in model space.
    std::vector<int> triangleMaterials;     // One valid index into `materials` per triangle.
    std::vector<ImportedMaterial> materials;
    std::vector<ImportedTexture> textures;
    size_t meshCount = 0;
    size_t embeddedTextureCount = 0;
    std::string formatName;                 // Human-readable importer name, e.g. "Autodesk FBX".
};

/**
 * Whether the V texture coordinate should be flipped when sampling, for a given file extension.
 *
 * Image loaders hand back the top row first, so the flip is needed exactly when the format places
 * v = 0 at the *bottom* of the image. OBJ, FBX, Collada and 3DS all do (the OpenGL convention);
 * glTF 2.0 is the odd one out and specifies a top-left origin, so it must not be flipped. Getting
 * this wrong is not subtle — the model samples the mirrored half of its atlas and comes out wearing
 * another material's colors.
 *
 * @param extension File extension including the dot, any case (e.g. ".glb").
 * @return bool True if V should be flipped for that format.
 */
inline bool defaultFlipVForFormat(const std::string& extension) {
    std::string lowered = extension;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return !(lowered == ".gltf" || lowered == ".glb");
}

/**
 * Every file extension the linked Assimp build can read, space separated (e.g. "*.obj *.fbx …").
 * Queried from the library rather than hardcoded, so it cannot drift from what actually works.
 * @return std::string The extension list.
 */
inline std::string supportedExtensions() {
    Assimp::Importer importer;
    aiString extensions;
    importer.GetExtensionList(extensions);
    return std::string(extensions.C_Str());
}

namespace detail {

/**
 * Indexes every file under a directory by lowercased filename, so a texture can be found even when
 * the model names it with a path that no longer exists — an artist's absolute `C:\Users\…\tex.png`,
 * or a case that only matched on a case-insensitive filesystem. Both are extremely common in the
 * wild, particularly in FBX exports.
 */
class FileIndex {
public:
    explicit FileIndex(std::filesystem::path root) : root_(std::move(root)) {}

    /**
     * Finds a file by name, ignoring case and any directories in the reference.
     * @param fileName The filename to look for; leading directories are ignored.
     * @return std::filesystem::path The matching file, or an empty path if there is none.
     */
    const std::filesystem::path find(const std::string& fileName) {
        if (!built_) build();
        auto it = filesByName_.find(toLower(fileName));
        return it == filesByName_.end() ? std::filesystem::path() : it->second;
    }

private:
    static std::string toLower(const std::string& text) {
        std::string lowered = text;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        return lowered;
    }

    void build() {
        built_ = true;
        std::error_code error;
        if (root_.empty() || !std::filesystem::is_directory(root_, error)) return;

        // Bounded so that pointing the tool at a huge asset tree cannot turn a missing texture into
        // a full-disk walk.
        constexpr int MAX_INDEXED_FILES = 50000;
        int indexed = 0;
        auto options = std::filesystem::directory_options::skip_permission_denied;
        for (std::filesystem::recursive_directory_iterator it(root_, options, error), end;
             it != end && indexed < MAX_INDEXED_FILES; it.increment(error)) {
            if (error) break;
            if (!it->is_regular_file(error)) continue;
            filesByName_.emplace(toLower(it->path().filename().string()), it->path());
            indexed++;
        }
    }

    std::filesystem::path root_;
    std::unordered_map<std::string, std::filesystem::path> filesByName_;
    bool built_ = false;
};

/**
 * Turns a texture reference from a model file into a path that exists on disk.
 *
 * References are unreliable by nature: they carry Windows separators, absolute paths from a machine
 * that is not this one, or a directory layout that did not survive being zipped up. The search
 * therefore widens in stages — the reference as written, then relative to the asset root and the
 * model's own directory, then the bare filename in the usual texture subdirectories, and finally a
 * case-insensitive search of the whole asset tree.
 *
 * @param reference The texture path as written in the model file.
 * @param modelDirectory Directory containing the model file.
 * @param assetRoot Root of the asset tree to search.
 * @param index Lazily-built filename index over `assetRoot`.
 * @return std::filesystem::path An existing file, or an empty path if nothing matched.
 */
inline std::filesystem::path resolveTexturePath(const std::string& reference,
                                                const std::filesystem::path& modelDirectory,
                                                const std::filesystem::path& assetRoot,
                                                FileIndex& index) {
    if (reference.empty()) return {};

    // Normalize Windows separators; std::filesystem treats a backslash as an ordinary character on
    // POSIX, so a Windows-authored path would otherwise become one absurdly long filename.
    std::string normalized = reference;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    while (normalized.rfind("./", 0) == 0) normalized.erase(0, 2);

    std::filesystem::path referencePath(normalized);
    std::string fileName = referencePath.filename().string();

    std::vector<std::filesystem::path> candidates;
    candidates.push_back(referencePath);
    if (!assetRoot.empty()) candidates.push_back(assetRoot / referencePath);
    if (!modelDirectory.empty()) candidates.push_back(modelDirectory / referencePath);

    // The reference's directories are frequently stale even when the file is right there.
    const char* subdirectories[] = {"", "textures", "Textures", "tex", "maps", "images", "source"};
    for (const std::filesystem::path& base : {assetRoot, modelDirectory}) {
        if (base.empty()) continue;
        for (const char* subdirectory : subdirectories) {
            candidates.push_back(base / subdirectory / fileName);
        }
    }

    std::error_code error;
    for (const std::filesystem::path& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate, error)) return candidate;
    }

    return index.find(fileName);
}

/**
 * Copies an embedded texture out of the scene. Assimp stores these one of two ways: `mHeight == 0`
 * means the payload is still a compressed file (png/jpg/…) of `mWidth` bytes, otherwise it is a raw
 * image whose texels are ordered B, G, R, A.
 * @param texture The embedded texture to copy.
 * @param name Reference name to record for logging.
 * @return ImportedTexture The copied texture.
 */
inline ImportedTexture copyEmbeddedTexture(const aiTexture& texture, const std::string& name) {
    ImportedTexture imported;
    imported.name = name;

    if (texture.mHeight == 0) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(texture.pcData);
        imported.encodedBytes.assign(bytes, bytes + texture.mWidth);
        return imported;
    }

    imported.rawWidth = int(texture.mWidth);
    imported.rawHeight = int(texture.mHeight);
    imported.rawPixels.resize(size_t(texture.mWidth) * texture.mHeight * 4);
    for (size_t texel = 0; texel < size_t(texture.mWidth) * texture.mHeight; texel++) {
        imported.rawPixels[texel * 4 + 0] = texture.pcData[texel].r;
        imported.rawPixels[texel * 4 + 1] = texture.pcData[texel].g;
        imported.rawPixels[texel * 4 + 2] = texture.pcData[texel].b;
        imported.rawPixels[texel * 4 + 3] = texture.pcData[texel].a;
    }
    return imported;
}

/**
 * Reads a material's diffuse color and diffuse texture, registering the texture in the model.
 * PBR-based formats (glTF, and FBX written by modern exporters) express these as base color, while
 * the classic formats use the Phong diffuse slot, so both are tried.
 */
inline ImportedMaterial importMaterial(const aiScene& scene, const aiMaterial& material,
                                       const std::filesystem::path& modelDirectory,
                                       const std::filesystem::path& assetRoot,
                                       FileIndex& index, ImportedModel& model,
                                       std::unordered_map<std::string, int>& texturesByReference,
                                       int& missingTextures) {
    ImportedMaterial imported;

    aiString materialName;
    if (material.Get(AI_MATKEY_NAME, materialName) == AI_SUCCESS) {
        imported.name = materialName.C_Str();
    }

    aiColor4D color;
    if (material.Get(AI_MATKEY_BASE_COLOR, color) == AI_SUCCESS ||
        material.Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS) {
        imported.diffuseColor = {color.r * 255.0f, color.g * 255.0f, color.b * 255.0f};
    }

    aiString texturePath;
    bool hasTexture = material.GetTexture(aiTextureType_BASE_COLOR, 0, &texturePath) == AI_SUCCESS ||
                      material.GetTexture(aiTextureType_DIFFUSE, 0, &texturePath) == AI_SUCCESS;
    if (!hasTexture || texturePath.length == 0) return imported;

    std::string reference = texturePath.C_Str();
    auto existing = texturesByReference.find(reference);
    if (existing != texturesByReference.end()) {
        imported.textureIndex = existing->second;
        return imported;
    }

    // GetEmbeddedTexture resolves both the `*0` index form and a name that happens to match an
    // embedded texture, which is how GLB and binary FBX carry their images.
    if (const aiTexture* embedded = scene.GetEmbeddedTexture(texturePath.C_Str())) {
        model.textures.push_back(copyEmbeddedTexture(*embedded, reference));
        model.embeddedTextureCount++;
    } else {
        std::filesystem::path resolved =
            resolveTexturePath(reference, modelDirectory, assetRoot, index);
        if (resolved.empty()) {
            projv::core::warn("  [MISSING] '{}' — no matching file under the asset directory", reference);
            missingTextures++;
            texturesByReference[reference] = -1; // Remember the miss so it is reported once.
            return imported;
        }
        ImportedTexture texture;
        texture.name = reference;
        texture.path = resolved;
        model.textures.push_back(std::move(texture));
    }

    imported.textureIndex = int(model.textures.size()) - 1;
    texturesByReference[reference] = imported.textureIndex;
    return imported;
}

/**
 * Registers a material's height/bump map, for displacement. Returns the texture index or -1.
 *
 * Which slot holds it is format-dependent and inconsistent: OBJ's `map_Bump`/`bump` lands in
 * aiTextureType_HEIGHT, `disp` in DISPLACEMENT, and glTF-style assets use NORMALS. All three are
 * tried in that order.
 *
 * The subtlety is that a "bump" reference frequently names a *normal* map rather than a height map —
 * a tangent-space RGB field, which is a gradient and not a displacement, and cannot be used as one
 * without integrating it. Asset packs that were converted to normal maps commonly keep the artist's
 * original grayscale height map right beside it under the same name minus a prefix (San Miguel ships
 * `N_moldura2piso_bump.png` next to `moldura2piso_bump.png`, for all 57 of its bump materials). So a
 * leading `N_` is stripped and the sibling preferred when it exists.
 *
 * Whether what we end up with is *actually* grayscale is verified later from its pixels, where the
 * image is decoded — a filename is a hint, not proof.
 */
inline int importHeightTexture(const aiScene& scene, const aiMaterial& material,
                              const std::filesystem::path& modelDirectory,
                              const std::filesystem::path& assetRoot,
                              FileIndex& index, ImportedModel& model,
                              std::unordered_map<std::string, int>& texturesByReference) {
    aiString texturePath;
    bool hasBump = material.GetTexture(aiTextureType_HEIGHT, 0, &texturePath) == AI_SUCCESS ||
                   material.GetTexture(aiTextureType_DISPLACEMENT, 0, &texturePath) == AI_SUCCESS ||
                   material.GetTexture(aiTextureType_NORMALS, 0, &texturePath) == AI_SUCCESS;
    if (!hasBump || texturePath.length == 0) return -1;

    std::string reference = texturePath.C_Str();

    // Prefer the grayscale sibling: same reference with a leading `N_` dropped from the filename.
    //
    // Split on either separator by hand rather than through std::filesystem. A reference written on
    // Windows arrives as `textures\N_foo.png`, and on POSIX std::filesystem treats the backslash as
    // an ordinary character — so filename() hands back the whole string and a prefix test against it
    // silently never matches.
    size_t separator = reference.find_last_of("/\\");
    std::string directoryPart = (separator == std::string::npos) ? std::string() : reference.substr(0, separator + 1);
    std::string filename = (separator == std::string::npos) ? reference : reference.substr(separator + 1);

    std::vector<std::string> candidates;
    if (filename.rfind("N_", 0) == 0 || filename.rfind("n_", 0) == 0) {
        candidates.push_back(directoryPart + filename.substr(2));
    }
    candidates.push_back(reference);

    for (const std::string& candidate : candidates) {
        auto existing = texturesByReference.find(candidate);
        if (existing != texturesByReference.end()) {
            if (existing->second >= 0) return existing->second;
            continue; // Remembered miss.
        }
        if (const aiTexture* embedded = scene.GetEmbeddedTexture(candidate.c_str())) {
            model.textures.push_back(copyEmbeddedTexture(*embedded, candidate));
            model.embeddedTextureCount++;
        } else {
            std::filesystem::path resolved =
                resolveTexturePath(candidate, modelDirectory, assetRoot, index);
            if (resolved.empty()) {
                // Not warned about: a missing sibling is the normal case, and a missing bump map only
                // costs displacement rather than color.
                texturesByReference[candidate] = -1;
                continue;
            }
            ImportedTexture texture;
            texture.name = candidate;
            texture.path = resolved;
            model.textures.push_back(std::move(texture));
        }
        int textureIndex = int(model.textures.size()) - 1;
        texturesByReference[candidate] = textureIndex;
        return textureIndex;
    }
    return -1;
}

} // namespace detail

/**
 * Loads a model of any format Assimp can read and flattens it into a world-space triangle soup.
 *
 * @param modelPath Path to the model file.
 * @param assetRoot Directory to search for external textures; the model's own directory is always
 *                  searched too. May be empty.
 * @param model Receives the imported geometry, materials and texture references.
 * @param missingTextures Receives the number of texture references that could not be resolved.
 * @return bool True on success; false if the file could not be read or held no triangles.
 */
inline bool importModel(const std::filesystem::path& modelPath,
                        const std::filesystem::path& assetRoot, ImportedModel& model,
                        int& missingTextures) {
    using namespace projv::core;

    Assimp::Importer importer;

    // Drop everything the voxelizer will never read. Animations and bones in particular can dwarf
    // the geometry in a rigged FBX, and none of it survives voxelization.
    importer.SetPropertyInteger(AI_CONFIG_PP_RVC_FLAGS,
                                aiComponent_COLORS | aiComponent_BONEWEIGHTS | aiComponent_ANIMATIONS |
                                aiComponent_LIGHTS | aiComponent_CAMERAS | aiComponent_TANGENTS_AND_BITANGENTS);
    // Pivot nodes exist to reproduce Maya/3ds Max transform semantics during animation. Baking the
    // hierarchy makes them pure overhead — and they multiply node counts enough to matter.
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);

    const unsigned int postProcessFlags =
        aiProcess_Triangulate |            // n-gons and quads become triangles
        aiProcess_PreTransformVertices |   // bake the node hierarchy into world space
        aiProcess_SortByPType |            // isolate points/lines so they can be skipped
        aiProcess_GenUVCoords |            // materialize spherical/cylindrical UV mappings
        aiProcess_TransformUVCoords |      // bake UV transforms into the coordinates
        aiProcess_FindDegenerates |        // flag zero-area triangles
        aiProcess_FindInvalidData |        // drop NaNs and other junk
        aiProcess_RemoveComponent |
        aiProcess_JoinIdenticalVertices |
        aiProcess_OptimizeMeshes;

    const aiScene* scene = importer.ReadFile(modelPath.string(), postProcessFlags);
    if (scene == nullptr || scene->mRootNode == nullptr) {
        error("Failed to load model: {}", importer.GetErrorString());
        return false;
    }
    if ((scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0) {
        warn("Model loaded but the importer reported it as incomplete — some content may be missing.");
    }

    // Report which importer actually handled the file. Assimp picks by content when the extension
    // lies, so this is worth surfacing rather than echoing the extension back.
    std::string extension = modelPath.extension().string();
    size_t importerIndex = extension.empty() ? size_t(-1) : importer.GetImporterIndex(extension.c_str());
    if (importerIndex != size_t(-1)) {
        if (const aiImporterDesc* description = importer.GetImporterInfo(importerIndex)) {
            model.formatName = description->mName;
        }
    }
    if (model.formatName.empty()) {
        model.formatName = extension.empty() ? "unknown" : extension.substr(1);
    }

    std::filesystem::path modelDirectory = modelPath.parent_path();
    detail::FileIndex fileIndex(assetRoot.empty() ? modelDirectory : assetRoot);
    std::unordered_map<std::string, int> texturesByReference;
    missingTextures = 0;

    if (scene->mNumMaterials > 0) info("Resolving materials and textures...");
    model.materials.reserve(scene->mNumMaterials);
    for (unsigned int materialIndex = 0; materialIndex < scene->mNumMaterials; materialIndex++) {
        ImportedMaterial imported = detail::importMaterial(
            *scene, *scene->mMaterials[materialIndex], modelDirectory, assetRoot, fileIndex, model,
            texturesByReference, missingTextures);
        // Registered after the diffuse map so a bump reference that happens to name the same file as
        // some material's diffuse map reuses that entry rather than decoding the image twice.
        imported.heightTextureIndex = detail::importHeightTexture(
            *scene, *scene->mMaterials[materialIndex], modelDirectory, assetRoot, fileIndex, model,
            texturesByReference);
        model.materials.push_back(std::move(imported));
    }

    // A triangle always needs a material to read its fallback color from, even when the format
    // carries none at all (STL, most PLY files).
    int fallbackMaterial = -1;
    auto fallbackMaterialIndex = [&]() {
        if (fallbackMaterial < 0) {
            fallbackMaterial = int(model.materials.size());
            ImportedMaterial material;
            material.name = "default";
            model.materials.push_back(material);
        }
        return fallbackMaterial;
    };

    size_t skippedNonTriangles = 0;
    for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; meshIndex++) {
        const aiMesh& mesh = *scene->mMeshes[meshIndex];

        // aiProcess_SortByPType splits mixed meshes by primitive type; a point or line mesh has no
        // area and therefore nothing to voxelize.
        if ((mesh.mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0) {
            skippedNonTriangles += mesh.mNumFaces;
            continue;
        }
        model.meshCount++;

        int materialIndex = mesh.mMaterialIndex < model.materials.size()
                                ? int(mesh.mMaterialIndex)
                                : fallbackMaterialIndex();
        const bool hasTexCoords = mesh.HasTextureCoords(0);

        for (unsigned int faceIndex = 0; faceIndex < mesh.mNumFaces; faceIndex++) {
            const aiFace& face = mesh.mFaces[faceIndex];
            if (face.mNumIndices != 3) continue; // Degenerates that triangulation could not fix.

            for (unsigned int corner = 0; corner < 3; corner++) {
                unsigned int index = face.mIndices[corner];
                ImportedVertex vertex;
                vertex.position = {mesh.mVertices[index].x, mesh.mVertices[index].y,
                                   mesh.mVertices[index].z};
                vertex.texCoord = hasTexCoords
                                      ? vec2{mesh.mTextureCoords[0][index].x,
                                             mesh.mTextureCoords[0][index].y}
                                      : vec2{0.0f, 0.0f};
                model.vertices.push_back(vertex);
            }
            model.triangleMaterials.push_back(materialIndex);
        }
    }

    if (skippedNonTriangles > 0) {
        warn("Skipped {} point/line primitive(s) — they have no surface to voxelize.", skippedNonTriangles);
    }
    if (model.triangleMaterials.empty()) {
        error("No triangles found in model. Aborting.");
        return false;
    }

    return true;
}

} // namespace meshimport

#endif // PROJECTV_MESH_IMPORT_HPP
