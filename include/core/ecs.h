#ifndef PROJECTV_ECS_H
#define PROJECTV_ECS_H

#include <cstdint>
#include <typeindex>
#include <any>
#include <unordered_map>
#include <functional>
#include <iostream>
#include "data_structures/application.h"

#include "core/log.h"

namespace projv::core {
    World createWorld();

    // Creats an application with default startup, update, and render functions.
    Application createApp();

    // Adds an entity to our world.
    Entity createEntity(World& world);

    template<typename T>
    T& createGlobalResource(World& world) {
        std::type_index typeIndex = std::type_index(typeid(T));
        auto it = world.globalResources.find(typeIndex);
        if (it == world.globalResources.end()) {
            std::any resource(std::in_place_type<T>);
            auto [insertedIt, _] = world.globalResources.emplace(typeIndex, std::move(resource));
            return std::any_cast<T&>(insertedIt->second);
        } else {
            std::cout << "Resource already exists!" << std::endl;
            return std::any_cast<T&>(it->second);
        }
    }
    
    template<typename T>
    void deleteGlobalResource(World& world) {
        std::type_index typeIndex = std::type_index(typeid(T));
        world.globalResources.erase(typeIndex);
    }

    // fetches the component storage for a specific type, creates it if it doesn't exist.
    template<typename T>
    ComponentStorage<T>& getOrCreateStorage(World& world) {
        std::type_index type = std::type_index(typeid(T));
        auto it = world.componentStorages.find(type);
        if (it == world.componentStorages.end()) {
            auto [newIt, _] = world.componentStorages.emplace(type, ComponentStorage<T>());
            return std::any_cast<ComponentStorage<T>&>(newIt->second);
            world.componentStorages[type] = ComponentStorage<T>();
        } else {
            return std::any_cast<ComponentStorage<T>&>(it->second);
        }
    }

    // Attempts to get the component storage for  specific type, returns nullptr if it doesn't exist.
    template<typename T>
    ComponentStorage<T>* tryGetStorage(World& world) {
        auto it = world.componentStorages.find(std::type_index(typeid(T)));
        if (it == world.componentStorages.end()) {
            return nullptr;
        }
        return std::any_cast<ComponentStorage<T>>(&it->second);
    }
    // Adds a component to an entity.
    template<typename T>
    void addComponent(World& world, Entity& entity, T component){
        auto& storage = getOrCreateStorage<T>(world);
        storage[entity] = std::move(component);
    }

    // Adds a component to an entity.
    template<typename T>
    void removeComponent(World& world, Entity& entity){
        auto& storage = getOrCreateStorage<T>(world);
        storage.erase(entity);
    }

    // Checks if an entity has a component.
    template<typename T>
    bool hasComponent(World& world, Entity entity) {
        auto* storage = tryGetStorage<T>(world);
        return storage && storage->find(entity) != storage->end();
    }

    // Fetches a component from an entity.
    template<typename T>
    T& getComponent(World& world, Entity entity) {
        auto& storage = getOrCreateStorage<T>(world);
        return storage.at(entity);
    }

    template<typename T>
    T& getGlobalResource(World& world) {
        auto it = world.globalResources.find(std::type_index(typeid(T)));
        if (it == world.globalResources.end()) {
            std::cout << "Resource not found!" << std::endl;
        }
        return std::any_cast<T&>(it->second);
    }

    // Loops over all the entities with the specified components and calls the function.
    template<typename... Components, typename Func>
    void forEachEntityWith(World& world, Func func) {
        auto& primaryStorage = getOrCreateStorage<std::tuple_element_t<0, std::tuple<Components...>>>(world);
        for (auto& [entity, _] : primaryStorage) {
            if ((hasComponent<Components>(world, entity) && ...)) {
                func(entity, getComponent<Components>(world, entity)...);
            }
        }
    }

    // Adds a system to our application for a specific stage.
    void assignSystemStage(Application& app, SystemStage stage, std::function<void(Application&)> system);

    // Runs the specified application and starts the loop.
    void runApplication(Application& app);
}

#endif
