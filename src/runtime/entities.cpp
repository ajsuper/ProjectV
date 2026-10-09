#include "runtime/entities.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "core/log.h"
#include "utils/compose_io.h"
#include "utils/scene_query.h"

namespace projv::runtime {
    namespace {
        constexpr uint32_t ENTITIES_VERSION = 1;
        constexpr const char* ENTITIES_FILE = "entities.json";

        struct ComponentRegistry {
            std::map<std::string, detail::ComponentCodec> codecs;
        };

        ComponentRegistry& registryOf(World& world) {
            if (ComponentRegistry* registry = world.ctx().find<ComponentRegistry>()) return *registry;
            return world.ctx().emplace<ComponentRegistry>();
        }

        const ComponentRegistry* registryOf(const World& world) {
            return world.ctx().find<ComponentRegistry>();
        }

        // One folder instance: the node standing for it (INVALID for the top-level folder) and where
        // it is on disk.
        struct Document {
            ComponentHandle node = INVALID_COMPONENT_HANDLE;
            std::filesystem::path folder;
            int depth = 0;
        };

        int depthOf(const Scene& scene, ComponentHandle h) {
            int depth = 0;
            while (h != INVALID_COMPONENT_HANDLE && h < scene.components.size()) {
                h = scene.components[h].parent;
                depth++;
            }
            return depth;
        }

        bool standsForFolder(const Scene& scene, ComponentHandle h) {
            const ComponentRecord& record = scene.components[h];
            return record.kind == ComponentKind::Asset && !record.sourcePath.empty() &&
                   utils::isComponentAlive(scene, h);
        }

        void collectUnder(const Scene& scene, ComponentHandle h, std::vector<Document>& out) {
            if (h >= scene.components.size() || !utils::isComponentAlive(scene, h)) return;
            if (standsForFolder(scene, h)) out.push_back({h, scene.components[h].sourcePath, depthOf(scene, h)});
            for (ComponentHandle child : scene.components[h].children) collectUnder(scene, child, out);
        }

        LinkMode modeFor(const Scene& scene, ComponentHandle h) {
            return scene.components[h].parent == INVALID_COMPONENT_HANDLE ? LinkMode::Root : LinkMode::Part;
        }

        void applyComponents(World& world, Entity entity, const nlohmann::json& components,
                             const std::string& where) {
            if (!components.is_object()) return;
            ComponentRegistry& registry = registryOf(world);
            for (const auto& [key, value] : components.items()) {
                auto codec = registry.codecs.find(key);
                if (codec != registry.codecs.end() && codec->second.load(world, entity, value)) continue;
                if (codec != registry.codecs.end()) {
                    core::warn("entities: '{}' in {} could not be read and is kept as it was", key, where);
                }
                world.get_or_emplace<UnknownComponents>(entity).byKey[key] = value.dump();
            }
        }

        // Spawns one folder's entities.json, if it has one.
        void spawnDocument(World& world, Scene& scene, const Document& document, SpawnedEntities& out) {
            std::filesystem::path path = document.folder / ENTITIES_FILE;
            std::ifstream in(path);
            if (!in) return;
            nlohmann::json json = nlohmann::json::parse(in, nullptr, false, true);
            const std::string where = path.string();
            if (json.is_discarded() || !json.is_object()) {
                core::error("entities: {} is not valid JSON - skipped", where);
                return;
            }
            if (json.value("version", 0u) != ENTITIES_VERSION) {
                core::error("entities: {} has version {}, expected {} - skipped", where, json.value("version", 0u),
                            ENTITIES_VERSION);
                return;
            }
            if (!json.contains("entities") || !json["entities"].is_array()) return;

            uint32_t order = 0;
            for (const nlohmann::json& record : json["entities"]) {
                Authored authored;
                authored.document = document.node;
                if (document.node < scene.components.size()) authored.documentGeneration = scene.components[document.node].generation;
                authored.name = record.value("name", std::string());
                authored.order = order++;

                ComponentHandle target = INVALID_COMPONENT_HANDLE;
                if (record.contains("link")) {
                    const nlohmann::json& link = record["link"];
                    if (link.is_string() && link.get<std::string>() == "document") {
                        authored.link = Authored::Link::Document;
                        target = document.node;   // the top-level folder has no node: unlinked
                    } else if (link.is_number_unsigned()) {
                        authored.link = Authored::Link::Component;
                        authored.linkId = link.get<uint32_t>();
                        target = utils::findComponentByLocalId(scene, document.node, authored.linkId);
                        if (target == INVALID_COMPONENT_HANDLE) {
                            core::warn("entities: '{}' in {} links id {}, which this folder does not have - "
                                       "spawned unlinked", authored.name, where, authored.linkId);
                        }
                    }
                }
                OnUnlink onUnlink = record.value("onUnlink", std::string()) == "destroy" ? OnUnlink::Destroy
                                                                                          : OnUnlink::Keep;

                // A component already linked -- by an inner folder's file, spawned first -- takes
                // this record's components as additions and overrides, rather than a second entity
                // fighting it for the transform.
                Entity entity = target != INVALID_COMPONENT_HANDLE ? entityFor(world, target) : NullEntity;
                bool fresh = entity == NullEntity;
                if (fresh) {
                    entity = world.create();
                    world.emplace<Authored>(entity, authored);
                    if (target != INVALID_COMPONENT_HANDLE &&
                        !linkComponent(world, entity, target, modeFor(scene, target), onUnlink)) {
                        core::warn("entities: '{}' in {} could not be linked - spawned unlinked", authored.name, where);
                    }
                }
                if (record.contains("components")) applyComponents(world, entity, record["components"], where);
                if (fresh) out.entities.push_back(entity);
            }
        }

        SpawnedEntities spawnDocuments(World& world, std::vector<Document> documents) {
            SpawnedEntities spawned;
            Scene* scene = world.ctx().find<Scene>();
            if (!scene) {
                core::error("spawnEntities: there is no projv::Scene in world.ctx()");
                return spawned;
            }
            // Deepest first, so an outer folder's file overrides an inner one's.
            std::stable_sort(documents.begin(), documents.end(),
                             [](const Document& a, const Document& b) { return a.depth > b.depth; });
            for (const Document& document : documents) spawnDocument(world, *scene, document, spawned);
            return spawned;
        }
    }

    namespace detail {
        void addCodec(World& world, const std::string& key, ComponentCodec codec) {
            registryOf(world).codecs[key] = std::move(codec);
        }
    }

    SpawnedEntities spawnEntities(World& world) {
        registerComponent<Transform>(world);
        Scene* scene = world.ctx().find<Scene>();
        if (!scene) {
            core::error("spawnEntities: there is no projv::Scene in world.ctx()");
            return {};
        }
        std::vector<Document> documents;
        if (!scene->documentPath.empty()) documents.push_back({INVALID_COMPONENT_HANDLE, scene->documentPath, 0});
        for (ComponentHandle h = 0; h < scene->components.size(); h++) {
            if (scene->components[h].parent == INVALID_COMPONENT_HANDLE) collectUnder(*scene, h, documents);
        }
        return spawnDocuments(world, std::move(documents));
    }

    SpawnedEntities spawnEntitiesUnder(World& world, ComponentHandle node) {
        registerComponent<Transform>(world);
        Scene* scene = world.ctx().find<Scene>();
        if (!scene) {
            core::error("spawnEntitiesUnder: there is no projv::Scene in world.ctx()");
            return {};
        }
        std::vector<Document> documents;
        collectUnder(*scene, node, documents);
        return spawnDocuments(world, std::move(documents));
    }

    namespace {
        // Whether the cache's pin at `index` still holds the blob it copied. It always does while the
        // cache and the Scene live together -- a pinned blob cannot be freed, and an edit forks rather
        // than writing to it -- but the Scene can be replaced wholesale under the cache, and then the
        // index names whatever the new scene keeps there.
        bool stillPinned(const Scene& scene, int32_t index, const GeometryBlob& source) {
            if (index < 0 || static_cast<size_t>(index) >= scene.geometryPool.size()) return false;
            const GeometryBlob& blob = scene.geometryPool[index];
            return blob.refCount > 0 && blob.geometry == source.geometry && blob.materialIDs == source.materialIDs;
        }

        std::string cacheKey(const std::string& folder) {
            std::error_code error;
            std::filesystem::path canonical = std::filesystem::weakly_canonical(folder, error);
            return error ? folder : canonical.string();
        }
    }

    void clearPrefabCache(World& world) {
        PrefabCache* cache = world.ctx().find<PrefabCache>();
        if (!cache) return;
        if (Scene* scene = world.ctx().find<Scene>()) {
            for (auto& [folder, entry] : cache->byFolder) {
                for (size_t i = 0; i < entry.pinned.size(); i++) {
                    if (stillPinned(*scene, entry.pinned[i], entry.loaded.geometryPool[i])) {
                        releaseBlob(*scene, entry.pinned[i]);
                    }
                }
            }
        }
        cache->byFolder.clear();
    }

    Entity instantiatePrefab(World& world, const std::string& folder, core::vec3 position, core::quat rotation,
                             float scale, OnUnlink onUnlink) {
        Scene* scene = world.ctx().find<Scene>();
        if (!scene) {
            core::error("instantiatePrefab: there is no projv::Scene in world.ctx()");
            return NullEntity;
        }
        PrefabCache& cache = world.ctx().contains<PrefabCache>() ? world.ctx().get<PrefabCache>()
                                                                 : world.ctx().emplace<PrefabCache>();
        const std::string key = cacheKey(folder);
        auto found = cache.byFolder.find(key);
        if (found == cache.byFolder.end()) {
            Scene loaded = utils::loadComposeFromDisk(folder);
            if (loaded.components.empty()) {
                core::error("instantiatePrefab: {} loaded no components", folder);
                return NullEntity;
            }
            found = cache.byFolder.emplace(key, PrefabCache::Entry{std::move(loaded), {}}).first;
        }
        PrefabCache::Entry& entry = found->second;
        // Pins the Scene no longer holds are forgotten, not released: they are not the cache's any more.
        for (size_t i = 0; i < entry.pinned.size(); i++) {
            if (entry.pinned[i] >= 0 && !stillPinned(*scene, entry.pinned[i], entry.loaded.geometryPool[i])) {
                entry.pinned[i] = -1;
            }
        }
        ComponentHandle root = utils::instantiateSceneInto(*scene, entry.loaded,
                                                           std::filesystem::path(folder).filename().string(),
                                                           INVALID_COMPONENT_HANDLE, position, rotation, scale,
                                                           &entry.pinned);
        if (root == INVALID_COMPONENT_HANDLE) return NullEntity;
        spawnEntitiesUnder(world, root);

        Entity entity = entityFor(world, root);
        if (entity == NullEntity) {
            entity = world.create();
            if (!linkComponent(world, entity, root, LinkMode::Root, onUnlink)) {
                world.destroy(entity);
                return NullEntity;
            }
        } else if (VoxelComponent* link = world.try_get<VoxelComponent>(entity)) {
            link->onUnlink = onUnlink;   // the caller's ownership wins over the file's
        }
        return entity;
    }

    bool saveEntities(const World& world, ComponentHandle document, const std::string& folder) {
        const Scene* scene = world.ctx().find<Scene>();
        if (!scene) {
            core::error("saveEntities: there is no projv::Scene in world.ctx()");
            return false;
        }
        const ComponentRegistry* registry = registryOf(world);

        // Which entities belong to this folder -- each entity belongs to exactly one file. One that
        // came from a file belongs to that file. One made in code belongs to the file its component
        // is an entry of: the parent's. (Not the linked node's own file: a node is an entry of its
        // parent's folder *and* the document of its own, and claiming both writes it twice.)
        auto isEntryOfThisFolder = [&](ComponentHandle h) {
            return h < scene->components.size() && scene->components[h].parent == document;
        };
        struct Row { Entity entity; uint32_t order; };
        std::vector<Row> rows;
        for (auto entity : world.view<entt::entity>()) {
            const Authored* authored = world.try_get<Authored>(entity);
            const VoxelComponent* link = world.try_get<VoxelComponent>(entity);
            // An authored entity matches by ref: its folder's row may since have been freed and reused.
            bool mine = authored ? authored->document == document &&
                                       (document == INVALID_COMPONENT_HANDLE ||
                                        authored->documentGeneration == scene->components[document].generation)
                                 : (link && utils::isComponentAlive(*scene, link->ref()) && isEntryOfThisFolder(link->handle));
            if (mine) rows.push_back({entity, authored ? authored->order : UINT32_MAX});
        }
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.order != b.order) return a.order < b.order;
            return entt::to_integral(a.entity) < entt::to_integral(b.entity);
        });

        nlohmann::json entities = nlohmann::json::array();
        for (const Row& row : rows) {
            Entity entity = row.entity;
            const Authored* authored = world.try_get<Authored>(entity);
            const VoxelComponent* link = world.try_get<VoxelComponent>(entity);
            nlohmann::json record = nlohmann::json::object();
            if (authored && !authored->name.empty()) record["name"] = authored->name;

            bool linkedHere = link && (link->handle == document ? document != INVALID_COMPONENT_HANDLE
                                                                : isEntryOfThisFolder(link->handle));
            if (linkedHere) {
                if (link->handle == document) record["link"] = "document";
                else record["link"] = scene->components[link->handle].localId;
                if (link->onUnlink == OnUnlink::Destroy) record["onUnlink"] = "destroy";
            } else if (authored && authored->link == Authored::Link::Document) {
                record["link"] = "document";
            } else if (authored && authored->link == Authored::Link::Component) {
                record["link"] = authored->linkId;   // unresolved, kept as written
            }

            nlohmann::json components = nlohmann::json::object();
            if (registry) {
                for (const auto& [key, codec] : registry->codecs) {
                    // A linked entity's transform is its component's, saved in compose.json.
                    if (link && key == ComponentTraits<Transform>::key) continue;
                    if (std::optional<nlohmann::json> value = codec.save(world, entity)) components[key] = *value;
                }
            }
            if (const UnknownComponents* unknown = world.try_get<UnknownComponents>(entity)) {
                for (const auto& [key, text] : unknown->byKey) {
                    if (!components.contains(key)) components[key] = nlohmann::json::parse(text, nullptr, false);
                }
            }
            if (!components.empty()) record["components"] = std::move(components);
            entities.push_back(std::move(record));
        }

        std::error_code error;
        std::filesystem::create_directories(folder, error);
        std::ofstream out(std::filesystem::path(folder) / ENTITIES_FILE);
        if (!out) {
            core::error("saveEntities: could not write {}/{}", folder, ENTITIES_FILE);
            return false;
        }
        nlohmann::json file{{"version", ENTITIES_VERSION}, {"entities", std::move(entities)}};
        out << file.dump(2) << "\n";
        return bool(out);
    }
}

// ---- Transform ------------------------------------------------------------------------------

nlohmann::json projv::runtime::ComponentTraits<projv::Transform>::save(const projv::Transform& t) {
    return nlohmann::json{{"position", {t.position.x, t.position.y, t.position.z}},
                          {"rotation", {t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w}},
                          {"scale", t.scale}};
}

std::optional<projv::Transform> projv::runtime::ComponentTraits<projv::Transform>::load(const nlohmann::json& json,
                                                                                       uint32_t) {
    projv::Transform t;
    if (json.contains("position")) {
        const auto& p = json["position"];
        t.position = projv::core::vec3(p.at(0).get<float>(), p.at(1).get<float>(), p.at(2).get<float>());
    }
    if (json.contains("rotation")) {
        const auto& r = json["rotation"];
        t.rotation = projv::core::quat(r.at(3).get<float>(), r.at(0).get<float>(), r.at(1).get<float>(), r.at(2).get<float>());
    }
    t.scale = json.value("scale", 1.0f);
    return t;
}
