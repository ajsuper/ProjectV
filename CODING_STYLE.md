# ProjectV Coding Style and Formatting Guide

This document defines the coding style and formatting conventions for the ProjectV repository. All contributors should follow these guidelines to maintain consistency across the codebase.

## Table of Contents

1. [Naming Conventions](#naming-conventions)
2. [Formatting and Indentation](#formatting-and-indentation)
3. [Brace Placement](#brace-placement)
4. [Comments and Documentation](#comments-and-documentation)
5. [Include File Organization](#include-file-organization)
6. [Code Organization Patterns](#code-organization-patterns)
7. [Error Handling](#error-handling)
8. [Memory Management](#memory-management)
9. [Template Usage](#template-usage)
10. [Configuration Files](#configuration-files)

## Naming Conventions

### Variables and Functions
- **camelCase** for local variables and member variables
  ```cpp
  int entityCount;
  glm::vec3 cameraPosition;
  ```

- **camelCase** for functions
  ```cpp
  void createWorld();
  Voxel getVoxelAt(int x, int y, int z);
  ```

### Types and Classes
- **PascalCase** for structs, classes, and types
  ```cpp
  struct World {
      uint32_t nextEntityID;
  };
  
  class Application {
      // implementation
  };
  ```

### Constants and Macros
- **UPPER_SNAKE_CASE** for constants and macros
  ```cpp
  const int MAX_VOXELS_PER_CHUNK = 32;
  #define PROJECTV_VERSION_MAJOR 1
  ```

### Namespaces
- All code organized under `projv::` namespace with sub-namespaces
  ```cpp
  namespace projv {
  namespace core {
      // core functionality
  }
  namespace graphics {
      // graphics-related code
  }
  namespace utils {
      // utility functions
  }
  }
  ```

### Files
- **snake_case.cpp** and **snake_case.h** for source files
- Header guards use `PROJECTV_FILENAME_H` or `PROJV_MODULE_FILENAME_H` format
  ```cpp
  #ifndef PROJECTV_ECS_H
  #define PROJECTV_ECS_H
  
  // code
  
  #endif // PROJECTV_ECS_H
  ```

## Formatting and Indentation

### Indentation
- **4 spaces** for indentation (never tabs)
- Consistent indentation throughout all files

### Spacing
- Space after control flow keywords
  ```cpp
  if (condition) {
      // code
  }
  
  for (int i = 0; i < count; ++i) {
      // code
  }
  ```

- Space after commas in function calls and parameter lists
  ```cpp
  function(arg1, arg2, arg3);
  ```

- Spaces around binary operators
  ```cpp
  int result = a + b * c;
  bool isEqual = (x == y);
  ```

- No space before opening parentheses in function definitions
  ```cpp
  void functionName() {
      // code
  }
  ```

## Brace Placement

Use K&R style (same line) for all braces:
```cpp
void function() {
    if (condition) {
        // code
    } else {
        // code
    }
    
    for (int i = 0; i < count; ++i) {
        // code
    }
}

struct StructName {
    int member1;
    float member2;
};
```

## Comments and Documentation

### Inline Comments
- Use `//` for inline comments
- Place comments on the right side for closely related code
- Add blank lines between unrelated code blocks
  ```cpp
  // Initialize the world with default values
  world.nextEntityID = 0;
  world.entities.reserve(1000);
  
  // Create the main camera
  Camera mainCamera;
  ```

### Documentation Comments
- Use Doxygen style in header files with `/** ... */`
- Include `@param` and `@return` tags for functions
- Document all public interfaces in headers
  ```cpp
  /**
   * Creates a Voxel with the specified color and position.
   * @param color The color to assign to the voxel.
   * @param position The 3D integer coordinates where the voxel will be placed.
   * @return Voxel The newly created voxel with the specified attributes.
   */
  Voxel createVoxel(const glm::vec3& color, const glm::ivec3& position);
  ```

## Include File Organization

### Order of Includes
1. System headers (`<iostream>`, `<vector>`, `<memory>`, etc.)
2. External dependencies (`"spdlog/spdlog.h"`, `"bgfx/bgfx.h"`, `"glm/glm.hpp"`, etc.)
3. Project headers (local includes), using relative paths

### Include Style
- Use angle brackets for system and external headers
- Use quotes for local project includes
  ```cpp
  #include <iostream>
  #include <vector>
  #include <memory>
  
  #include "spdlog/spdlog.h"
  #include "bgfx/bgfx.h"
  #include "glm/glm.hpp"
  
  #include "core/ecs.h"
  #include "graphics/gpu_interface.h"
  #include "utils/voxel_management.h"
  ```

## Code Organization Patterns

### Functional Programming Approach
- Follow functional programming principles with minimal OOP features
- **Separation of concerns**: Structs store data, functions provide functionality
- **No constructors** in structs - use factory functions instead
- **No methods** inside structs - all functionality as separate functions

### Module Structure
- Each module (core, graphics, utils) has its own header and source directories
- Clear separation between interface (headers) and implementation
- Factory function pattern for object creation

### Example Structure
```cpp
// In header file (.h)
namespace projv {
namespace core {

struct World {
    uint32_t nextEntityID;
    std::vector<Entity> entities;
};

/**
 * Creates a new world with default parameters.
 * @return World A newly initialized world.
 */
World createWorld();

/**
 * Adds an entity to the specified world.
 * @param world The world to add the entity to.
 * @param entity The entity to add.
 * @return uint32_t The ID assigned to the new entity.
 */
uint32_t addEntity(World& world, const Entity& entity);

} // namespace core
} // namespace projv

// In source file (.cpp)
namespace projv {
namespace core {

World createWorld() {
    World world;
    world.nextEntityID = 0;
    world.entities.reserve(1000);
    return world;
}

uint32_t addEntity(World& world, const Entity& entity) {
    uint32_t id = world.nextEntityID++;
    world.entities.push_back(entity);
    return id;
}

} // namespace core
} // namespace projv
```

## Error Handling

### Exceptions
- Use standard exceptions for error handling
- Prefer `std::invalid_argument` for invalid parameters
- Use `std::runtime_error` for runtime failures
  ```cpp
  if (position.x < 0 || position.y < 0 || position.z < 0) {
      throw std::invalid_argument("Voxel position cannot be negative");
  }
  
  if (!gpuInterface->isInitialized()) {
      throw std::runtime_error("GPU interface not initialized");
  }
  ```

### Logging
- Use the project's logging system for debug and informational messages
  ```cpp
  #include "core/log.h"
  
  core::info("Successfully created voxel at position ({}, {}, {})", x, y, z);
  core::warn("Texture file not found, using default texture");
  core::error("Failed to initialize GPU interface");
  ```

## Memory Management

### Smart Pointers
- Use `std::shared_ptr` for shared resource management
- Use `std::unique_ptr` for exclusive ownership
- Follow RAII principles
  ```cpp
  auto renderer = std::make_shared<Renderer>(gpuInterface);
  auto textureData = std::make_unique<uint8_t[]>(textureSize);
  ```

### Move Semantics
- Use move semantics for performance where appropriate
  ```cpp
  Voxel createVoxel(glm::vec3 color, glm::ivec3 position) {
      Voxel voxel;
      voxel.color = std::move(color);
      voxel.position = std::move(position);
      return voxel;  // NRVO/move semantics
  }
  ```

## Template Usage

### Generic Programming
- Use templates for generic functionality
- Use `if constexpr` for compile-time type checking
- Provide template specializations for different data types
  ```cpp
  template<typename T>
  constexpr bool isVoxelType() {
      if constexpr (std::is_same_v<T, uint8_t>) {
          return true;
      } else if constexpr (std::is_same_v<T, uint16_t>) {
          return true;
      }
      return false;
  }
  ```

## Configuration Files

### .clang-format
The project recommends the following `.clang-format` configuration:
```yaml
---
Language: Cpp
BasedOnStyle: Google
Standard: c++17
IndentWidth: 4
UseTab: Never
ColumnLimit: 100
AccessModifierOffset: -2
AllowShortFunctionsOnASingleLine: Empty
AllowShortIfStatementsOnASingleLine: false
AllowShortLoopsOnASingleLine: false
BreakBeforeBraces: Attach
SpaceAfterCStyleCast: false
SpaceBeforeParens: ControlStatements
SpacesInParentheses: false
SpacesInSquareBrackets: false
```

### CMake Configuration
- Use **C++17** standard
- Enable all warnings: `-Wall -Wextra`
- Debug symbols and optimization: `-g -O3`
- Consistent library naming: `projectV-moduleName`

## Best Practices

1. **Keep functions small and focused** - Each function should do one thing well
2. **Use meaningful names** - Variable and function names should clearly indicate purpose
3. **Document public interfaces** - All public functions and structs should have proper documentation
4. **Be consistent** - Follow these conventions consistently across all files
5. **Write testable code** - Structure code to make it easy to unit test
6. **Use the logging system** - Prefer logging over printf/stdout for debugging
7. **Handle errors gracefully** - Use exceptions and proper error handling throughout

## Tools and Automation

- **Formatting**: Use `clang-format` with the provided configuration
- **Static Analysis**: Enable compiler warnings and consider using clang-tidy
- **Build**: Use the provided CMake configuration for consistent builds

---

This style guide should be followed by all contributors to maintain code consistency and readability across the ProjectV project. When in doubt, look at existing code for examples and prefer consistency over personal preference.