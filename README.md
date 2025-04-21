# ATTENTION!
ProjectV is under very early development, data structures and functionalities will change frequently so don't currently invest too much time into generating data or programs unless you plan on sticking with a version.

# ProjectV

'ProjectV' provides functionalities that allow for easy manipulation and rendering of voxel data.

# Contents

- [Goals of ProjectV](#Goals-of-ProjectV)
- [Installation](#installation)
- [Uninstallation](#uninstallation)
    - [Entire library](#entire-library)
    - [Only build files](#only-build-files)
- [Dependencies](#dependencies)
    - [Install Dependencies](#install-dependencies)
- [Modules](#modules)
    - [Utils](#utils)
    - [Core](#core)
    - [Graphics](#graphics)
- [Usage](#usage)
- [Contributing](#contributing)

### Goals of ProjectV

---

Voxels have unique properties that make them ideal for advanced rendering while providing a unique and charming aesthetic. This project aims to:

- Allow inexperienced programmers to experiment with rendering beautiful scenes without being reliant on a complex or high level game engine.
- Allow experienced programmers to leverage the provided functionalities to achieve performant and graphically impressive scenes. 
- Simplify the process of rendering 3D scenes.
- Create an open source voxel engine to allow for people to test their ideas.

### Installation

---

Currently only supports Linux

> **Warning** On MacOS, OpenGL support only goes up to 4.1, not the 4.6 that is required for the library to function. Execution will fail even though the dependencies are available. Windows is currently not supported, however Windows was in mind when choosing the libraries we use and it should be able to work just fine and should be coming soon.

Linux:
```bash
$ git clone https://github.com/ajsuper/ProjectV
$ cd /path/to/ProjectV
$ make
```

### Uninstallation

---

#### Entire library (Completely deletes the library):

> **Warning** rm -r can be dangerous when used incorrectly. Ensure it is used on the desired file path.
```bash
$ rm -r /path/to/ProjectV
```

#### Only build files (Keeps the source code and file structure):

```bash
$ cd /path/to/projectV
$ make clean
```

### Dependencies

---

Please ensure the latest of the following are installed to use.

- `glfw`
- `GLEW`
- `GL`

#### Install dependencies:

Linux:

```bash
$ sudo apt-get install libglfw3
$ sudo apt-get install libglfw3-dev
$ sudo apt-get install libglew-dev
```
Update graphics drivers for GL

### Modules

ProjectV uses a modular structure, each module contains functionalities you can use in your project.

> **Note** modules are not linked upon compilation of this project. They must be linked when you compile the program that uses them.

---
### Utils

Provides functions and data structures for handling voxel data. For more information and a full list of util modules see [utils.md](/include/utils/utils.md)

Include Statement Example:

```C++
#include "utils/voxel_math.h"
```

Linker Flag:
>-lprojectV-voxel_math

---
### Core

Provides functions for handling a projv::Application. For more information and a full list of core modules see [core.md](/include/core/core.md)

```C++
#include "core/ecs.h"
```

Linker Flag:
>-lprojectV-ecs

---
### Graphics

Provides functions to load, compile, and render shaders to an OpenGL GLFW window. Also provides functions to update camera postiion, resoltuion and pass data to be rendered to the shaders. For more information and a full list of core modules see [graphics.md](/include/graphics/graphics.md)

```C++
#include "graphics/render.h"
```

Linker Flag:
>-lprojctV-render

### Usage

Steps to setting up a project using ProjectV:

1. **Assess Necessary Modules**:
    - Review the documentation to determine which modules are required for your project. To assess necessary modules, please see: 
        - [utils.md](/include/utils/utils.md)
        - [core.md](/include/core/core.md)
        - [graphics.md](/include/graphics/graphics.md)

    > **Attention!** If you are using the graphics module, it is recommended to copy the source folder `shaders` from ProjectV since this includes highly complex shaders required for rendering.

2. **Include Header Files**:
    - Include the header files for the modules you want to use in your project. For example:
        ```cpp
        #include "utils/voxel_math.h"
        #include "core/ecs.h"
        #include "graphics/window.h"
        ```

3. **Namespace Usage**:
    - Ensure all references to ProjectV functionalities and variables use the `projv` namespace. For example:
        ```cpp
        projv::core::functionName();
        // or
        projv::utils::functionName();
        // or
        projv::graphics::functionName();
        ```

4. **Compile with Necessary Flags**:
    - Compile your project with the necessary linker flags for the modules you are using. For example:
        > **Note** ***Graphics*** requires `-lGL`, `-lglfw3`, and `-lGLEW`

For more detailed instructions in the form of commented code, please see our examples using the ProjectV. [Examples](/examples)

### Contributing

This is my first time open sourcing any project, and my first project on this scale. If there is anything that I should've done differently, or if something could be greatly improved, please feel free to change it or let me know! ProjectV is open source under the [MIT License](/LICENSE.md) and is welcoming as many contributors as possible! All contributions are welcome, whether it's features additions, optimizations, or just spelling corrections. If you choose to contribute, please visit the [MIT License](/LICENSE.md) and make sure you read the [CODE_OF_CONDUCT.md](/CODE_OF_CONDUCT.md). Also be sure to visit [CONTRIBUTING.md](/CONTRIBUTING.md) to get instructions on how to contribute! Thank you!!

#### Citations

Contributors must cite sources that require it in our [SOURCES.md](/SOURCES.md). You are not required to cite ProjectV when using it; however, it is highly recommended as the credit is appreciated and it helps people find out about the engine!

Thank you for using ProjectV!!
