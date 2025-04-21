# ProjectV Terrain Generation Example

This is an example program using **ProjectV** to generate a grid of chunks containing procedurally generated terrain.

### Features Demonstrated:

- Voxel data generation
- Voxel data saving and loading
- LOD (Level of Detail) updates
- ProjectV math utilities
- Global resource management
- Scene creation and GPU upload
- Basic user input handling
- Shader management
- Comprehensive Makefile setup

### Third-Party Libraries:

- **PerlinNoise**  
  Copyright (C) 2013–2021  
  Author: Ryo Suzuki  
  [GitHub Repository](https://github.com/Reputeless/PerlinNoise)

---

To run this example, build the project and launch the application. You will see a 3D terrain rendered using raymarching shaders with dynamically updating levels of detail based on camera distance.

### Build:

#### Linux:
```bash
#Bash shell
cd path/to/ProjectV/docs/examples/TerrainGenerationExample
mkdir build
cd build
cmake ..
make
cd ..
./build/TerrainGenerator
```


