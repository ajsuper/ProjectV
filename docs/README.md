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

> **Warning** On MacOS, OpenGL support only goes up to 4.1, not the 4.6 that is required for the library to function. Execution will fail even though the dependencies are available.

Linux:
```bash
$ git clone INSERT THE LINK
$ cd /path/to/ProjectV-vX.X
$ make
```

### Uninstallation

---

#### Entire library (Completely deletes the library):

> **Warning** rm -r can be dangerous when used incorrectly. Ensure it is used on the desired file path.
```bash
$ rm -r /path/to/ProjectV-vX.X
```

#### Only build files (Keeps the source code and file structure):

```bash
$ cd /path/to/projectV-vX.X
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

Provides functions and data structures for handling voxel data.

Linker Flag:
>-lprojctV-utils

Include Statement:

```C++
#include "utils.h"
```

---
### Core

Provides functions for creating an OpenGL instance and GLFW Window, along with handling user input.

Linker Flag:
>-lprojctV-core

```C++
#include "core.h"
```

---
### Graphics

Provides functions to load, compile, and render shaders to an OpenGL GLFW window. Also provides functions to update camera postiion, resoltuion and pass data to be rendered to the shaders.

Linker Flag:
>-lprojctV-graphics

```C++
#include "graphics.h"
```

### Usage

Steps to setting up a project using ProjectV:

1. **Assess Necessary Modules**:
    - Review the documentation to determine which modules are required for your project.

    > **Attention!** If you are using the graphics module, ensure the source file `shaders` from ProjectV is included in the same directory as the cpp file using it.

2. **Include Header Files**:
    - Include the header files for the modules you want to use in your project. For example:
        ```cpp
        #include "utils.h"
        #include "core.h"
        #include "graphics.h"
        ```

3. **Namespace Usage**:
    - Ensure all references to ProjectV functionalities and variables use the `projv` namespace. For example:
        ```cpp
        projv::functionName();
        ```

4. **Compile with Necessary Flags**:
    - Compile your project with the necessary linker flags for the modules you are using. For example:
        > **Note** ***Graphics*** requires `-lGL` and ***core*** requires `-lglfw3` and `-lGLEW`

        ```bash
        $ g++ -o myProject main.cpp -I/path/to/ProjectV-vX.X/include/ -L/path/to/ProjectV-vX.X/lib/ -lprojectV-utils -lprojectV-core -lprojectV-graphics -lglfw -lGL -lGLEW
        ```

For more detailed instructions in the form of commented code, please see our examples using the ProjectV. [Examples](/docs/examples)

### Contributing

ProjectV is open source under the [MIT License](/docs/LICENSE.md) and is welcoming as many contributors as possible! All contributions are welcome, whether it's features additions, optimizations, or just spelling corrections. If you choose to contribute, please visit the [MIT License](/docs/LICENSE.md). Also be sure to visit [CONTRIBUTING](/docs/CONTRIBUTING.md) to get instructions on how to contribute! Thank you!!

#### Citations

We highly encourage contributors to cite their sources in our [SOURCES.md](/docs/SOURCES.md). ProjectV does not require citations; However, they are greatly appreciated and reccommended if you intend for others to be looking at and editing your code. 

Thank you for using ProjectV!!

Last edited on: 31-12-2024
