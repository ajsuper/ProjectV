## SCENE - v0.0

The 'Scene' data structure is designed to allow for rendering and handling of multiple octrees.


### Structure in Memory
Each **Scene** object simply contains an std::vector<**RuntimeChunkData**>.
**RuntimeChunkData** is a structure than only exists during runtime and contains a **CPUChunkHeader**, **geometryData**, **voxelTypeData** and **LOD**
- **CPUChunkHeader** - contains the **chunkID** ***(Used to link to the geometry and voxelType data's)***, **position** ***(all 3 axis stored in one uint32_t)***, **scale**, and **resolution**
- **geometryData** - Simply the octree structure defined in [octree_data_structure.md](/docs/data_structures/octree_data_structure.md)
- **voxelTypeData** - Simply the voxel type data structure defined in [voxel_type_data_structure.md](/docs/data_structures/voxel_type_data_structure.md)
- **LOD** - A integer representing how many levels of detail the chunk has been lowered ***(0 is the highest, 2 is lower etc.)***.

### Structure in Disk
Files are as follows:
ComplexDataStructureTest
├── headers.json  
├── octree  
│   ├── 1.bin  
│   ├── 2.bin  
│   ├── 3.bin  
└── voxelTypeData  
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; ├── 1.bin  
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; ├── 2.bin  
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp; ├── 3.bin  
- **headers.json** contains the headers for a file. Please see [scene.h](/include/data_structures/scene.h)
- **octree** contains .bin files with the name being the corresponding chunkID
- **voxelTypeData** contains .bin files with the name being the corresponding chunkID.

#### Note
When passed to the shader, a new data structure is created that combines all of the data from structure in Memory into just 3 std::vector's

### More

For more information on this project, visit our [README.md](/docs/README.md)