#ifndef PROJECTV_ATTACHMENTS_H
#define PROJECTV_ATTACHMENTS_H

#include <any>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "core/log.h"
#include "data_structures/scene.h"
#include "nlohmann/json.hpp"

// Component attachments: data a program attaches to scene components and saves with them, which the
// engine stores, copies and writes back without interpreting. See AttachmentStore in scene.h for
// what the storage holds, and docs/data_structures/compose_data_structure.md for the file format.
//
// A program gives a type an attachment key by specialising AttachmentTraits:
//
//     struct CsgRole { BooleanOp op = BooleanOp::None; };
//     template<> struct projv::utils::AttachmentTraits<CsgRole> {
//         static constexpr const char*  key = "projv.editor.csg";
//         static constexpr uint32_t     version = 1;
//         static constexpr OnDuplicate  onDuplicate = OnDuplicate::Copy;
//         static nlohmann::json         save(const CsgRole&);
//         static std::optional<CsgRole> load(const nlohmann::json&, uint32_t fileVersion);
//     };
//
// `save` returns a JSON object; the "v" member is added on the way out and must not be set by it.
// `load` receives the whole stored value, "v" included, and the version it was written at, which is
// 1 when the file did not say. Returning nullopt (or throwing a nlohmann exception) refuses the
// value: it is logged once and kept as raw text, so it is still written back on save rather than
// lost because this build could not read it.
//
// Keys are namespaced strings. `projv.` is reserved for programs in this repository; anything else
// should use a prefix of its own.
//
// No type has to be registered before a scene is loaded. Entries arrive as JSON text and are decoded
// the first time their key is asked for as a type; a key nothing asks for is written back exactly as
// it was read.
//
// Main thread only.

namespace projv::utils {
    template<typename T>
    struct AttachmentTraits;   // Specialised by the program that owns T. No primary definition.

    namespace detail {
        // The scope's tables. Mutable members (see AttachmentStore), so a const Scene can hand them
        // out for the decode a typed read may do.
        std::map<std::string, AttachmentTable>& tablesFor(const Scene& scene, AttachmentScope scope);

        template<typename T>
        std::string encodeAttachment(const std::any& value) {
            nlohmann::json json = AttachmentTraits<T>::save(std::any_cast<const T&>(value));
            json["v"] = AttachmentTraits<T>::version;
            return json.dump();
        }

        // The key's table, bound to T: the first typed access records how T is written back and what
        // duplicating it does, and every access offers T any raw entries it has not seen yet.
        template<typename T>
        AttachmentTable& bindTable(const Scene& scene, AttachmentScope scope) {
            using Traits = AttachmentTraits<T>;
            AttachmentTable& table = tablesFor(scene, scope)[Traits::key];
            if (table.encode == nullptr) {
                table.encode = &encodeAttachment<T>;
                table.onDuplicate = Traits::onDuplicate;
            }
            if (!table.rawDecoded) {
                for (auto it = table.raw.begin(); it != table.raw.end(); ) {
                    std::optional<T> value;
                    nlohmann::json json = nlohmann::json::parse(it->second, nullptr, false);
                    if (!json.is_discarded()) {
                        uint32_t version = 1;
                        if (json.is_object() && json.contains("v") && json["v"].is_number_unsigned()) {
                            version = json["v"].template get<uint32_t>();
                        }
                        try {
                            value = Traits::load(json, version);
                        } catch (const nlohmann::json::exception&) {
                            value.reset();
                        }
                    }
                    if (value) {
                        table.typed[it->first] = std::move(*value);
                        it = table.raw.erase(it);
                    } else {
                        core::warn("attachments: '{}' on component {} could not be read and is kept "
                                   "as it was: {}", Traits::key, it->first, it->second);
                        ++it;
                    }
                }
                table.rawDecoded = true;
            }
            return table;
        }
    }

    // By key, typed or raw. For code that has no type for the key.
    bool hasAttachment(const Scene& scene, ComponentHandle handle, const std::string& key,
                       AttachmentScope scope = AttachmentScope::Component);
    void removeAttachment(Scene& scene, ComponentHandle handle, const std::string& key,
                          AttachmentScope scope = AttachmentScope::Component);

    // Every attachment on `handle`, in both scopes. Whoever deletes a component calls this: the
    // engine has no delete of its own (a deleted record is tombstoned by the program that deleted
    // it), and saveComposeToDisk skips tombstones regardless.
    void clearAttachments(Scene& scene, ComponentHandle handle);

    // Copies `from`'s attachments onto `to`, in both scopes, honouring each key's OnDuplicate.
    // duplicateComponent calls it for every node it copies.
    void duplicateAttachments(Scene& scene, ComponentHandle from, ComponentHandle to);

    // Key -> JSON text: the form compose_io reads and writes (ComposeComponent::attachments,
    // ComposeDoc::attachments).
    using AttachmentTexts = std::map<std::string, std::string>;

    // Everything on `handle` in `scope`, typed values encoded through their traits and raw ones
    // exactly as read.
    AttachmentTexts attachmentsForSave(const Scene& scene, ComponentHandle handle, AttachmentScope scope);

    // Adds entries as raw text, replacing whatever `handle` had under the same keys. Decoded when
    // their key is next asked for as a type.
    void attachRaw(Scene& scene, ComponentHandle handle, AttachmentScope scope, const AttachmentTexts& texts);

    // The attachment of type T on `handle`, or nullptr if it has none.
    template<typename T>
    T* getAttachment(Scene& scene, ComponentHandle handle,
                     AttachmentScope scope = AttachmentScope::Component) {
        AttachmentTable& table = detail::bindTable<T>(scene, scope);
        auto it = table.typed.find(handle);
        return it == table.typed.end() ? nullptr : std::any_cast<T>(&it->second);
    }

    template<typename T>
    const T* getAttachment(const Scene& scene, ComponentHandle handle,
                           AttachmentScope scope = AttachmentScope::Component) {
        AttachmentTable& table = detail::bindTable<T>(scene, scope);
        auto it = table.typed.find(handle);
        return it == table.typed.end() ? nullptr : std::any_cast<T>(&it->second);
    }

    // Sets (replacing any previous value, typed or raw) and returns the stored value.
    template<typename T>
    T& setAttachment(Scene& scene, ComponentHandle handle, T value,
                     AttachmentScope scope = AttachmentScope::Component) {
        AttachmentTable& table = detail::bindTable<T>(scene, scope);
        table.raw.erase(handle);
        std::any& slot = table.typed[handle];
        slot = std::move(value);
        return std::any_cast<T&>(slot);
    }

    template<typename T>
    void removeAttachment(Scene& scene, ComponentHandle handle,
                          AttachmentScope scope = AttachmentScope::Component) {
        removeAttachment(scene, handle, AttachmentTraits<T>::key, scope);
    }

    // Carries every attachment in `source` into `destination`, moving each handle through `remap`.
    // instantiateComposeInto uses it to graft one scene into another. An entry `remap` sends to
    // INVALID_COMPONENT_HANDLE in Component scope is dropped; Document scope maps INVALID (the
    // grafted folder's own block) like any other handle.
    template<typename Remap>
    void appendAttachments(Scene& destination, const Scene& source, Remap remap);
}

// ---- appendAttachments ---------------------------------------------------------------------

namespace projv::utils {
    namespace detail {
        void appendStore(AttachmentStore& destination, const AttachmentStore& source,
                         AttachmentScope scope, ComponentHandle (*remap)(void*, ComponentHandle),
                         void* context);
    }

    template<typename Remap>
    void appendAttachments(Scene& destination, const Scene& source, Remap remap) {
        auto trampoline = [](void* context, ComponentHandle handle) -> ComponentHandle {
            return (*static_cast<Remap*>(context))(handle);
        };
        detail::appendStore(destination.attachments, source.attachments,
                            AttachmentScope::Component, trampoline, &remap);
        detail::appendStore(destination.documentAttachments, source.documentAttachments,
                            AttachmentScope::Document, trampoline, &remap);
    }
}

#endif
