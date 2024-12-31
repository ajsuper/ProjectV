//Last edited on: 31-12-2024

/*
This example demonstrates how you can create an octree and its corresponding voxel type data and store them into binary files.
*/

#include "utils.h" // Only uses functions and data types from utils so only it is necessary.

int main(){
    std::vector<std::vector<std::vector<projv::voxel>>> voxels = projv::createVoxelGrid(128); // Creates a 512x512x512 voxel grid.

    for(int x = 0; x < 128; x++){ // Loops over all the voxels and fills them based off of a funciton.
        for(int y = 0; y < 128; y++){
            for(int z = 0; z < 128; z++){
                if(sin(float(x)/10) + sin(float(y)/10) + sin(float(z)/10) > 0){
                    voxels[x][y][z].filled = true;
                    voxels[x][y][z].color.r = x; //Assigns colors (0-255)
                    voxels[x][y][z].color.g = y;
                    voxels[x][y][z].color.b = z;
                }
            }
        }
    }

    std::vector<uint32_t> octree = projv::createOctree(voxels, 128, 0); // Converts the voxel grid into an octree.
    std::vector<uint32_t> voxelTypeData = projv::createVoxelTypeData(voxels, 128); // Generates the voxel type data for the voxels. (Stores colors)

    projv::writeUint32Vector(octree, "octree.bin"); // Writes the octree to a file.
    projv::writeUint32Vector(voxelTypeData, "voxelTypeData.bin"); // Writes the voxel type data to a file.

    std::vector<uint32_t> readOctree = projv::readUint32Vector("octree.bin"); // Reads the same octree from a file.
    std::vector<uint32_t> readVoxelTypeData = projv::readUint32Vector("voxelTypeData.bin"); // Reads the same voxel type data from a file.

    for(int index = 0; index < readOctree.size(); index++){ // Compares each item in our octrees to ensure they match.
        if(readOctree[index] != octree[index]){
            std::cout << "Octree data does not match" << std::endl;
            return -1;
        }
    }

    std::cout << "Octree data matches" << std::endl;

    for(int index = 0; index < readVoxelTypeData.size(); index++){ // Compares each item in our voxelTypeDatas to ensure they match.
        if(readVoxelTypeData[index] != voxelTypeData[index]){
            std::cout << "Voxel Type Data does not match" << std::endl;
            return -1;
        }
    }

    std::cout << "Voxel Type Data matches" << std::endl;

    return 0;
}