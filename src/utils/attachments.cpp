#include "utils/attachments.h"

namespace projv::utils {
    namespace detail {
        std::map<std::string, AttachmentTable>& tablesFor(const Scene& scene, AttachmentScope scope) {
            return scope == AttachmentScope::Document ? scene.documentAttachments.tables
                                                      : scene.attachments.tables;
        }

        void appendStore(AttachmentStore& destination, const AttachmentStore& source,
                         AttachmentScope scope, ComponentHandle (*remap)(void*, ComponentHandle),
                         void* context) {
            for (const auto& [key, from] : source.tables) {
                AttachmentTable& to = destination.tables[key];
                // A table this scene has not bound yet learns how to write the incoming typed values
                // from the scene they came from. Both tables are bound to the same T if both are
                // bound at all: a key names one type within a program.
                if (to.encode == nullptr && from.encode != nullptr) {
                    to.encode = from.encode;
                    to.onDuplicate = from.onDuplicate;
                }
                for (const auto& [handle, value] : from.typed) {
                    ComponentHandle mapped = remap(context, handle);
                    if (mapped == INVALID_COMPONENT_HANDLE && scope == AttachmentScope::Component) continue;
                    to.raw.erase(mapped);
                    to.typed[mapped] = value;
                }
                for (const auto& [handle, text] : from.raw) {
                    ComponentHandle mapped = remap(context, handle);
                    if (mapped == INVALID_COMPONENT_HANDLE && scope == AttachmentScope::Component) continue;
                    to.typed.erase(mapped);
                    to.raw[mapped] = text;
                    to.rawDecoded = false;
                }
            }
        }
    }

    bool hasAttachment(const Scene& scene, ComponentHandle handle, const std::string& key,
                       AttachmentScope scope) {
        const auto& tables = detail::tablesFor(scene, scope);
        auto table = tables.find(key);
        if (table == tables.end()) return false;
        return table->second.typed.count(handle) != 0 || table->second.raw.count(handle) != 0;
    }

    void removeAttachment(Scene& scene, ComponentHandle handle, const std::string& key,
                          AttachmentScope scope) {
        auto& tables = detail::tablesFor(scene, scope);
        auto table = tables.find(key);
        if (table == tables.end()) return;
        table->second.typed.erase(handle);
        table->second.raw.erase(handle);
    }

    void clearAttachments(Scene& scene, ComponentHandle handle) {
        for (AttachmentStore* store : { &scene.attachments, &scene.documentAttachments }) {
            for (auto& [key, table] : store->tables) {
                table.typed.erase(handle);
                table.raw.erase(handle);
            }
        }
    }

    void duplicateAttachments(Scene& scene, ComponentHandle from, ComponentHandle to) {
        if (from == to) return;
        for (AttachmentStore* store : { &scene.attachments, &scene.documentAttachments }) {
            for (auto& [key, table] : store->tables) {
                // Anything `to` already had is replaced, not merged: a duplicate is a copy of `from`.
                table.typed.erase(to);
                table.raw.erase(to);
                if (table.onDuplicate == OnDuplicate::Drop) continue;

                auto typed = table.typed.find(from);
                if (typed != table.typed.end()) {
                    std::any copy = typed->second;   // Copied first: the insert below may rehash.
                    table.typed[to] = std::move(copy);
                }
                auto raw = table.raw.find(from);
                if (raw != table.raw.end()) {
                    std::string copy = raw->second;
                    table.raw[to] = std::move(copy);
                }
            }
        }
    }

    AttachmentTexts attachmentsForSave(const Scene& scene, ComponentHandle handle, AttachmentScope scope) {
        AttachmentTexts texts;
        for (const auto& [key, table] : detail::tablesFor(scene, scope)) {
            auto typed = table.typed.find(handle);
            if (typed != table.typed.end() && table.encode != nullptr) {
                texts[key] = table.encode(typed->second);
                continue;
            }
            auto raw = table.raw.find(handle);
            if (raw != table.raw.end()) texts[key] = raw->second;
        }
        return texts;
    }

    void attachRaw(Scene& scene, ComponentHandle handle, AttachmentScope scope, const AttachmentTexts& texts) {
        auto& tables = detail::tablesFor(scene, scope);
        for (const auto& [key, text] : texts) {
            AttachmentTable& table = tables[key];
            table.typed.erase(handle);
            table.raw[handle] = text;
            table.rawDecoded = false;
        }
    }
}
