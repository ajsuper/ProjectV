// Last edited on: 31-12-2024.
// Data structures explained in (docs/data_structures/octree_data_structure.md) and (docs/data_structures/voxel_type_data_structure.md).

#include "utils.h"

namespace projv{
    
    std::vector<std::vector<std::vector<voxel>>> createVoxelGrid(int size) {
        // Initialize the 3D vector with `voxel` instances.
        std::vector<std::vector<std::vector<voxel>>> voxels(
            size, std::vector<std::vector<voxel>>(
                size, std::vector<voxel>(size, voxel{})));

        // Set default values of white and empty for each voxel.
        for (int x = 0; x < size; x++) {
            for (int y = 0; y < size; y++) {
                for (int z = 0; z < size; z++) {
                    voxels[x][y][z].filled = false;
                    voxels[x][y][z].color.r = 255;
                    voxels[x][y][z].color.g = 255;
                    voxels[x][y][z].color.b = 255;
                }
            }
        }

        return voxels;
    }

    uint32_t convertVec3ToHeaderPosition(uint32_t x, uint32_t y, uint32_t z){
        return (x & 0x3FF) | ((y & 0x3FF) << 10) | ((z & 0x3FF) << 20);
    }

    std::array<int, 3> convertHeaderPositionToVec3(uint32_t headerPosition){
        return {headerPosition & 0x3FF, (headerPosition >> 10) & 0x3FF, (headerPosition >> 20) & 0x3FF};
    }

    void writeUint32Vector(std::vector<uint32_t> vector, std::string fileDirectory){
        std::ofstream outFile(fileDirectory, std::ios::binary);
        if(!outFile){
            std::cout << "ERROR in 'writeUint32Vector': Failed to open " << fileDirectory << std::endl;
            return;
        }
        size_t size = vector.size();
        outFile.write(reinterpret_cast<const char*>(&size), sizeof(size));
        outFile.write(reinterpret_cast<const char*>(vector.data()), vector.size() * sizeof(uint32_t));
        outFile.close();
    }

    std::vector<uint32_t> readUint32Vector(std::string fileDirectory){
        std::ifstream inFile(fileDirectory, std::ios::binary);  
        if (!inFile) {
            std::cerr << "Error in 'readUint32Vector': Failed to open" << std::endl;
        }
        size_t size;
        inFile.read(reinterpret_cast<char*>(&size), sizeof(size));

        std::vector<uint32_t> readNumbers(size);

        inFile.read(reinterpret_cast<char*>(readNumbers.data()), size * sizeof(uint32_t));
        inFile.close();

        for (uint32_t num : readNumbers) {
            //std::cout << std::bitset<32>(num) << std::endl;
        } 
        return readNumbers; 
    }

    void writeHeadersJSON(const std::vector<chunkHeader>& chunkHeaders, const std::string& fileDirectory) {
        nlohmann::json jsonOutput;
        jsonOutput["chunkHeaders"] = nlohmann::json::array();

        for (const auto& header : chunkHeaders) {
            jsonOutput["chunkHeaders"].push_back({
                {"ID", header.ID},
                {"position x", convertHeaderPositionToVec3(header.position)[0]},
                {"position y", convertHeaderPositionToVec3(header.position)[1]},
                {"position z", convertHeaderPositionToVec3(header.position)[2]},
                {"scale", header.scale},
                {"resolution", header.resolution},
                {"geometryStartIndex", header.geometryStartIndex},
                {"geometryEndIndex", header.geometryEndIndex},
                {"voxelTypeDataStartIndex", header.voxelTypeDataStartIndex},
                {"voxelTypeDataEndIndex", header.voxelTypeDataEndIndex}
            });
        }

        std::ofstream outFile(fileDirectory);
        if (!outFile) {
            std::cerr << "ERROR in 'writeHeadersJSON': Failed to open " << fileDirectory << std::endl;
            return;
        }

        outFile << jsonOutput.dump(4);  // Pretty-print with 4 spaces for readability
        outFile.close();
    }

    std::vector<chunkHeader> readHeadersJSON(const std::string& fileDirectory) {
        std::ifstream inFile(fileDirectory);
        if (!inFile) {
            std::cerr << "Error in 'readHeadersJSON': Failed to open " << fileDirectory << std::endl;
            return {};
        }

        nlohmann::json jsonInput;
        inFile >> jsonInput;  // Read JSON file into json object
        inFile.close();

        std::vector<chunkHeader> headers;
        if (!jsonInput.contains("chunkHeaders") || !jsonInput["chunkHeaders"].is_array()) {
            std::cerr << "Error in 'readHeadersJSON': Invalid JSON format" << std::endl;
            return {};
        }

        for (const auto& jHeader : jsonInput["chunkHeaders"]) {
            chunkHeader header;
            header.ID = jHeader.value("ID", 0);
            uint x = jHeader.value("position x", 0);
            uint y = jHeader.value("position y", 0);
            uint z = jHeader.value("position z", 0);
            header.position = convertVec3ToHeaderPosition(x, y, z);
            header.scale = jHeader.value("scale", 0);
            header.resolution = jHeader.value("resolution", 0);
            header.geometryStartIndex = jHeader.value("geometryStartIndex", 0);
            header.geometryEndIndex = jHeader.value("geometryEndIndex", 0);
            header.voxelTypeDataStartIndex = jHeader.value("voxelTypeDataStartIndex", 0);
            header.voxelTypeDataEndIndex = jHeader.value("voxelTypeDataEndIndex", 0);
            
            headers.push_back(header);
        }

        return headers;
    }

    void writeSceneMetaData(scene scene, std::string fileDirectory){
        nlohmann::json jsonOutput;

        jsonOutput["meta"] = nlohmann::json::array();

        jsonOutput["meta"].push_back({
            {"version", scene.version}
        });

        std::ofstream outFile(fileDirectory);
        if (!outFile) {
            std::cerr << "ERROR in 'writeSceneMetaData': Failed to open " << fileDirectory << std::endl;
            return;
        }

        outFile << jsonOutput.dump(4);  // Pretty-print with 4 spaces for readability
        outFile.close();
    }

    void writeScene(scene scene, std::string fileDirectory){
        std::cout << "Writing scene to " << fileDirectory << std::endl;

        // Writes the chunk headers.
        std::cout << "Writing scene with " << scene.chunkHeaders.size()*(sizeof(uint32_t)*8)/4 << " bytes of chunk headers" << std::endl;
        writeUint32Vector(scene.serializedGeometry, fileDirectory + "/geometry.bin");

        // Writes our octrees.
        std::cout << "Writing scene with " << scene.serializedGeometry.size()*(sizeof(uint32_t))/4 << " bytes of geometry" << std::endl;
        writeUint32Vector(scene.serializedVoxelTypeData, fileDirectory + "/voxelTypeData.bin");
        
        // Writes the voxelTypeData.
        std::cout << "Writing scene with " << scene.serializedVoxelTypeData.size()*(sizeof(uint32_t))/4 << " bytes of voxel type data" << std::endl;
        writeHeadersJSON(scene.chunkHeaders, fileDirectory + "/headers.json");

        return;
    }

    scene readScene(std::string fileDirectory){
        scene scene;
        scene.serializedGeometry = readUint32Vector(fileDirectory + "/geometry.bin");
        scene.serializedVoxelTypeData = readUint32Vector(fileDirectory + "/voxelTypeData.bin");
        scene.chunkHeaders = readHeadersJSON(fileDirectory + "/headers.json");
        return scene;
    }

    /**
     * Finds how many filled voxels are within boundsMin and boundsMax in the 3D voxel vector.
     * 
     * @param voxels A 3D vector of voxel's.
     * @param voxelWholeResolution The resolution of the entire voxels vector.
     * @param boundsMin The smaller of the 2 bounds to check against.
     * @param boundsMax The larger of the 2 bounds to check against.
     * @return Returns the number of voxels within the bounds.
     */
    int voxInBounds(std::vector<std::vector<std::vector<voxel>>>& voxels, int wholeVoxelResolution, int boundsMin[3], int boundsMax[3]){
        int voxCountInBounds = 0;

        for (int x = 0; x < 3; x++){
            if(boundsMax[x] > wholeVoxelResolution || boundsMin[x] < 0){
                return 0;
            }
        }

        for(int x = boundsMin[0]; x < boundsMax[0]; x++){
            for(int y = boundsMin[1]; y < boundsMax[1]; y++){
                for(int z = boundsMin[2]; z < boundsMax[2]; z++){
                    if(voxels[x][y][z].filled){
                        voxCountInBounds += 1;
                    }
                }
            }
        }

        return voxCountInBounds;
    }

    /**
     * Calculates the masks for a section of voxels based off of the minimum xyz. The depthResolution/2 gives the size of the child nodes and is used to create the octants corresponding to their parent.
     * 
     * @param voxels A 3D vector of voxel's.
     * @param depthResolution The resolution of the voxel grid at a certain depth. Depth 1 is 2x2x2, depth 2 is 4x4x4. Essentially pow(2, depth)
     * @param wholeVoxelResolution The resolution of the voxel grid at its highest resolution. A voxel grid of 256^3 would be 256^3.
     * @return A 9 bit mask stored in a uint16_t. The rightmost bit is to indicate if the nodes are leaf nodes. The 8 to the left of that indicate whether they are non-empty nodes.
     */
    uint16_t buildMask(std::vector<std::vector<std::vector<voxel>>>& voxels, const int& checkX, const int& checkY, const int& checkZ, const int& depthResolution, int wholeVoxelResolution){
        // TODO: combine checkX, checkY, and checkZ into one check array or vec3.
        uint16_t mask = 0;
        int childSize = int(depthResolution/2);
        int x = 0;
        int y = 1;
        int z = 2;

        int octants[8][3] = {
            {checkX, checkY, checkZ},
            {checkX, checkY, checkZ+childSize},
            {checkX, checkY+childSize, checkZ},
            {checkX, checkY+childSize, checkZ+childSize},
            {checkX+childSize, checkY, checkZ},
            {checkX+childSize, checkY, checkZ+childSize},
            {checkX+childSize, checkY+childSize, checkZ},
            {checkX+childSize, checkY+childSize, checkZ+childSize}
        };

        // TODO: replace size check with more sophisticated check allowing for large filled sections to be sparseified just like large empty sections. Complex, involvwes render changes.
        // If child is not at leaf resolution.
        if(childSize > 1){
            for(int i = 0; i < 8; i++){
                int octantPosition[3] = {octants[i][x], octants[i][y], octants[i][z]};
                int octantPositionExtents[3] = {octants[i][x]+childSize, octants[i][y]+childSize, octants[i][z]+childSize};
                if(voxInBounds(voxels, wholeVoxelResolution, octantPosition, octantPositionExtents) > 0){
                    mask |= (1 << (8-i));
                }
            }
        }

        // If child is at leaf resolution.
        if(childSize == 1){
            for(int i = 0; i < 8; i++){
                int octantPosition[3] = {octants[i][x], octants[i][y], octants[i][z]};
                int octantPositionExtents[3] = {octants[i][x]+childSize, octants[i][y]+childSize, octants[i][z]+childSize};
                if(voxInBounds(voxels, wholeVoxelResolution, octantPosition, octantPositionExtents) > 0){
                    mask = mask | (1 << (8-i));
                    mask = mask | (1);
                }
            }
        }

        return mask;
    }

    /**
     * Creates the Z-Order Index of a point in 3D space with a certain bit depth.
     * 
     * @param x, y, z Position in 3D space to calculate Z-Order of.
     * @param bitDepth How many bits the index takes up.
     * @return Retruns the Z-Order
     */
    uint64_t createZOrderIndex(const int& x, const int& y, const int& z, const int& bitDepth){
        uint64_t z_order = 0;

        // TODO: Correct the ordering/naming. For some reason it only works when flipped. Changes requried in generator and renderer.
        for (int i = 0; i < bitDepth; ++i) {
            // Swapping x and y axes
            uint64_t bitY = (z >> i) & 1;
            uint64_t bitX = (y >> i) & 1; 
            uint64_t bitZ = (x >> i) & 1;

            // Interleave them with swapped x and y (y -> x -> z).
            z_order |= (bitY << (3 * i)) | (bitX << (3 * i + 1)) | (bitZ << (3 * i + 2));
        }

        return z_order;
    }

    /**
     * Creates a 3D point from a Z-Order index and the number of bits the index takes up.
     * 
     * @param z_order The Z-Order of the point
     * @param bitDepth The number of bits the index takes up.
     * @return Returns an std::array<int, 3> containg the 3D point given from the Z-Order index.
     */
    std::array<int, 3> reverseZOrderIndex(uint64_t z_order, int bitDepth) {
        int x = 0;
        int y = 0;
        int z = 0;

        // TODO: Correct the ordering. For some reason it only works when flipped. Changes requried in generator and renderer.
        for (int i = 0; i < bitDepth; ++i) {
            // Extract bits from z_order in the swapped y -> x -> z pattern.
            uint64_t bitY = (z_order >> (3 * i)) & 1; 
            uint64_t bitX = (z_order >> (3 * i + 1)) & 1; 
            uint64_t bitZ = (z_order >> (3 * i + 2)) & 1;

            // Reconstruct the original x, y, and z by setting the appropriate bits.
            z |= (bitY << i);  // bitY came from z.
            y |= (bitX << i);  // bitX came from y.
            x |= (bitZ << i);  // bitZ came from x.
        }
        return {x, y, z};
    }

    /**
     * Build all of the masks for the current depth in the octree. Does not return anything, but modifies the octree parameter to add the masks.
     * 
     * @param octree An empty std::vector<uint32_t> that contains the nodes of our octree.
     * @param voxels A 3D vector of voxel's to generate the masks from.
     * @param depthInOctree The depth at which we are building masks.
     * @param voxelWholeResolution The resolution of the entire 3D voxel vector.
     */
    void buildMasksForWholeDepth(std::vector<uint32_t>& octree, std::vector<std::vector<std::vector<voxel>>>& voxels, int depthInOctree, int voxelWholeResolution){
        int voxelDepthResolution = pow(2, depthInOctree);
        int wholeResToDepthResRatio = voxelWholeResolution/voxelDepthResolution;
        int index = 0;
        int bitLength = log2(voxelDepthResolution);

        //Loop over each of the coordinates and create a mask then add that to the whole list of masks.
        for(uint64_t i = 0; i < (voxelDepthResolution*voxelDepthResolution*voxelDepthResolution); i++){
            std::array<int, 3> coord = reverseZOrderIndex(i, bitLength);
            
            uint16_t temporaryMask = buildMask(voxels, wholeResToDepthResRatio*coord[0], wholeResToDepthResRatio*coord[1], wholeResToDepthResRatio*coord[2], wholeResToDepthResRatio, voxelWholeResolution);
            //std::cout << std::bitset<16>(temporaryMask) << " " << coord[0]*wholeResToDepthResRatio << " " << coord[1]*wholeResToDepthResRatio << " " << coord[2]*wholeResToDepthResRatio << " " << std::endl;
            if(temporaryMask != 0){
                octree.push_back(temporaryMask);
            }
        }
    }

    /**
     * Generate the relative child pointers and combine them with a partially constructed octree containing only the masks.
     * 
     * @param octree An std::vector<uint32_t> that has all of it's masks to which we are going to be adding the pointers.
     * @return Returns an optional reference to the octree.
     */
    std::vector<uint32_t> addPointers(std::vector<uint32_t>& octree){
        // TODO: Remove the return of the reference and make it only modify the octree directly.
        uint16_t vldMaskMask = 0b111111110;
        uint16_t lefMaskMask = 0b000000001;
        int parentCount = 0;
        int childCounter = 0;
        for(int address = 0; address < octree.size(); address++){
            uint32_t* current = &octree[address];
            uint32_t childPointer;
            uint8_t validMask = (*current & vldMaskMask) >> 1;
            uint8_t leafMask = (*current & lefMaskMask);
            if(validMask != 0 && leafMask == 0){ 
                childPointer = childCounter-address+1;    
                
                parentCount += 1;
                for(uint8_t i = 0; i < 8; i++){
                    if((validMask & (1 << i)) != 0){
                        childCounter += 1;
                    }
                }
            } else {
                childPointer = 0;
            }
            if(childPointer >= 0b11111111111111111111111){
                std::cerr << "[OCTREE SUB] [addPointers] Child pointer too large!" << std::bitset<32>(childPointer) << std::endl;
            }
            octree[address] = (*current & 0b111111111) | (childPointer << 9);
        }
        return octree;
    }

    /**
     * Generates an octree following a modified version of the version from Nvidia's paper "Effecient Sparse Voxel Octrees"
     * following this format [23-bit relative child pointer][8-bit valid mask][1-bit leaf flag]
     * 
     * @param voxels An std::vector<std::vector<std::vector<voxel>>>& containing the scene to be converted to a sparse voxel octree.
     * @param voxelWholeResolution
     * @param octreeID 
     * @return Returns an std::vector<uint32_t> containing the entire octree structure.
     */
    std::vector<uint32_t> createOctree(std::vector<std::vector<std::vector<voxel>>>& voxels, int voxelWholeResolution, uint32_t octreeID){
        auto startWhole = std::chrono::high_resolution_clock::now();
        int levelsOfDepth = int(log2(voxelWholeResolution));

        std::cerr << "[OCTREE MAIN] [createOctree] Generating raw octree..." << std::endl;
        std::cerr << "[OCTREE MAIN] [createOctree] Total depths: " << levelsOfDepth << std::endl;
        std::cerr << "[OCTREE MAIN] [createOctree] Voxel grid resolution: " << voxelWholeResolution << std::endl;
        std::cerr << "[OCTREE MAIN] [createOctree] Generating masks..." << std::endl;
        std::vector<uint32_t> octree;
        //octree.emplace_back(octreeID);
        for(int depthInOctree = 0; depthInOctree < levelsOfDepth; depthInOctree++){
            std::cerr << "[OCTREE SUB] [createOctree] Depth completions/Depth: " << ((depthInOctree+0.001)/levelsOfDepth)*100 << "% | " << depthInOctree << std::endl;
            buildMasksForWholeDepth(octree, voxels, depthInOctree, voxelWholeResolution);
        }
        auto end = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double, std::milli>(end - startWhole).count();
        std::cerr << "[OCTREE SUB] [createOctree] Depth completions: 100%" << std::endl;
        std::cerr << "[OCTREE SUB] [createOctree] Masks finished in: " << elapsed << " ms\n" << std::endl;  
        
        std::cerr << "[OCTREE MAIN] [createOctree] Generating pointers..." << std::endl;
        octree = addPointers(octree);
        end = std::chrono::high_resolution_clock::now();
        elapsed = std::chrono::duration<double, std::milli>(end - startWhole).count();
        std::cerr << "[OCTREE MAIN] [createOctree] Octree finished in: " << elapsed << " ms\n" << std::endl;

        return octree;
    }

    /**
     * Generates voxel type data that stores 8 bytes of data per voxel. Currently, a color and 3 8 bit sections of extra data.
     * 
     * @param voxels The 3D voxel std::vector that contains all of the voxels in the scene.
     * @param voxelWholeResolution The resolution of the entire 3D voxel vector.
     * @param voxelCount Optional paramater that reserves memory space to potentially decrease generation times from re-reserving during creation. 
     */
    std::vector<uint32_t> createVoxelTypeData(std::vector<std::vector<std::vector<voxel>>>& voxels, int voxelWholeResolution, int voxelCount) {
        std::cerr << "[VTD MAIN] [createVoxelTypeData] Creating voxel type data..." << std::endl;
        std::vector<uint32_t> voxelTypeData;
        auto startWhole = std::chrono::high_resolution_clock::now();
        if(voxelCount > 0){
            voxelTypeData.reserve(voxelCount*3);
        }
        std::cerr << "[VTD SUB] [createVoxelTypeData] Check Point #1" << std::endl;
        int bitLength = log2(voxelWholeResolution); // Number of bits for the coordinate
        //Loop over each of the coordinates and create a mask then add that to the whole list of masks.
        for(int i = 0; i < (voxelWholeResolution*voxelWholeResolution*voxelWholeResolution ); i++){
            std::array<int, 3> coord = reverseZOrderIndex(i, bitLength);
            if(voxels[coord[0]][coord[1]][coord[2]].filled){
                voxel voxel = voxels[coord[0]][coord[1]][coord[2]];
                //std::cout << i << "ZOrderIndex" << std::endl;
                voxelTypeData.emplace_back(i);
                int x = coord[0];
                int y = coord[1];
                int z = coord[2];
                int R10 = (voxel.color.r)*4;
                int G10 = (voxel.color.g)*4;
                int B10 = (voxel.color.b)*4;
                float normalX = 0;
                float normalY = 0;
                float normalZ = 0;
                uint8_t normalXSign = 1;
                uint8_t normalYSign = 1;
                uint8_t normalZSign = 1;
                if(normalX < 0) normalXSign = 0;
                if(normalY < 0) normalYSign = 0;
                if(normalZ < 0) normalZSign = 0;
                int normalX9 = int(abs(normalX)*511) & 0x1FF;
                int normalY9 = int(abs(normalY)*511) & 0x1FF;
                int normalZ9 = int(abs(normalZ)*511) & 0x1FF;
                uint32_t SerializedColor = uint32_t(R10 << 20 | G10 << 10 | B10);
                uint32_t SerializedNormal = uint32_t((normalXSign << 29) | (normalX9 << 20) | (normalYSign << 19) | (normalY9 << 10) | (normalZSign << 9) | normalZ9);
                voxelTypeData.emplace_back(SerializedColor);
                voxelTypeData.emplace_back(SerializedNormal);
            }
        }    
        std::cerr << "[VTD SUB] Size of vector: " << voxelTypeData.size() << std::endl;
        std::cerr << "[VTD SUB] [createVoxelTypeData] Check Point #2" << std::endl;
        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration<double, std::milli>(end - startWhole).count();
        std::cerr << "[VTD MAIN] [createVoxelTypeData] Voxel type data finished in: " << elapsed << " ms\n" << std::endl;
        return voxelTypeData;
    }

    // Temporary test scene generation function.
    bool isInSierpinskiTriangle(int x, int y, int z) {
        while(x > 0 || y > 0 || z > 0){
            x /= 2;
            y /= 2;
            z /= 2;
            if(x % 2 == 1 && y % 2 == 1 && z % 2 == 1){
                return false;
            }
        }
        return true;
    }

    // Temporary test scene generation function.
    bool isInSierpinskiCube(int x, int y, int z) {

        int depth = 10;
        // Adjust depth based on the size to make sure fractal levels fit within the grid.
        for(int i = 0; i < depth; ++i){
            // If x, y, or z modulo 4 equals 2, we are in a "void" region at this level.
            if((x % 4 == 2 && y % 4 == 2) || (y % 4 == 2 && z % 4 == 2) || (x % 4 == 2 && z % 4 == 2)){
                return false;
            }
            
            // Scale down coordinates to simulate the recursive check.
            x /= 2;
            y /= 2;
            z /= 2;
        }

        // If the point has passed through all levels without falling into a void.
        return true;
    }
}