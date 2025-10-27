#include "utils/voxel_io.h"

namespace projv::utils {
    void writeUint32Vector(std::vector<uint32_t> vector, std::string fileDirectory){
        // Ensure the directory exists
        std::filesystem::path pathDirectory = std::filesystem::path(fileDirectory).parent_path();
        if (!std::filesystem::exists(pathDirectory)) {
            std::filesystem::create_directories(pathDirectory);
        }

        std::ofstream outFile(fileDirectory, std::ios::binary);
        if (!outFile) {
            core::warn("Function: writeUint32Vector. Failed to open {}", fileDirectory);
            return;
        }
    
        size_t size = vector.size();
        outFile.write(reinterpret_cast<const char*>(&size), sizeof(size));
    
        if (size > 0) {
            outFile.write(reinterpret_cast<const char*>(vector.data()), size * sizeof(uint32_t));
        }
    
        outFile.close(); // Always creates the file
    }

    std::vector<uint32_t> readUint32Vector(std::string fileDirectory){
        core::info("Function: readUint32Vector. Reading uint32_t vector from {}", fileDirectory);
        std::ifstream inFile(fileDirectory, std::ios::binary);  
        if (!inFile) {
            core::error("Function: readUint32Vector. Failed to open {}", fileDirectory);
            return {};
        }
        size_t size;
        inFile.read(reinterpret_cast<char*>(&size), sizeof(size));

        std::vector<uint32_t> readNumbers(size);

        inFile.read(reinterpret_cast<char*>(readNumbers.data()), size * sizeof(uint32_t));
        inFile.close();

        return readNumbers; 
    }

    void writeHeadersJSON(const std::vector<ChunkHeader>& chunkHeaders, const std::string& fileDirectory) {
        core::info("Function: writeHeadersJSON. Writing headers to {}", fileDirectory);
        nlohmann::json jsonOutput;
        jsonOutput["chunkHeaders"] = nlohmann::json::array();
    
        for (const auto& header : chunkHeaders) {
            jsonOutput["chunkHeaders"].push_back({
                {"ID", header.chunkID},
                {"position x", header.position.x},
                {"position y", header.position.y},
                {"position z", header.position.z},
                {"voxel scale", header.voxelScale},
                {"resolution", header.resolution},
            });
        }
    
        // Create parent directory if needed
        std::filesystem::path path(fileDirectory);
        std::filesystem::create_directories(path.parent_path());
    
        std::ofstream outFile(fileDirectory);
        if (!outFile) {
            core::warn("Function: writeHeadersJSON. Still failed to open {}", fileDirectory);
            return;
        }
    
        outFile << jsonOutput.dump(4);
        outFile.close();
    }
    
    std::vector<ChunkHeader> readHeadersJSON(const std::string& fileDirectory) {
        core::info("Function: readHeadersJSON. Reading headers from {}", fileDirectory);
        std::ifstream inFile(fileDirectory);
        if (!inFile) {
            core::error("Function: readHeadersJSON. Failed to open {}", fileDirectory);
            return {};
        }

        nlohmann::json jsonInput;
        inFile >> jsonInput;  // Read JSON file into json object
        inFile.close();

        std::vector<ChunkHeader> headers;
        headers.reserve(jsonInput["chunkHeaders"].size());
        if (!jsonInput.contains("chunkHeaders") || !jsonInput["chunkHeaders"].is_array()) {
            std::cerr << "Error in 'readHeadersJSON': Invalid JSON format" << std::endl;
            return {};
        }

        for (const auto& jHeader : jsonInput["chunkHeaders"]) {
            ChunkHeader header;
            header.chunkID = jHeader.value("ID", 0);
            float x = jHeader.value("position x", 0);
            float y = jHeader.value("position y", 0);
            float z = jHeader.value("position z", 0);
            header.position = core::vec3(x, y, z);
            header.resolution = jHeader.value("resolution", 0); 
            header.voxelScale = jHeader.value("voxel scale", 0);
            header.scale = header.resolution * header.voxelScale * 0.0390625; // * 0.0390625 is to adjust it so that a voxel size of 1 and a resolution of 512 results in a chunk size of roughly 20. This is done for precision reasons.           
            headers.push_back(header);
        }

        return headers;
    }

    void writeSceneToDisk(std::string sceneFileDirectory, Scene& scene){
        core::info("Function: writeSceneToDisk. Writing scene to: {}", sceneFileDirectory);

        // Delete all files in the octree and voxelTypeData subdirectories
        std::filesystem::remove_all(sceneFileDirectory + "/");

        // Recreate the directories after deletion
        std::filesystem::create_directory(sceneFileDirectory);
        std::filesystem::create_directory(sceneFileDirectory + "/octree");
        std::filesystem::create_directory(sceneFileDirectory + "/voxelTypeData");

        // Delete the headers.json file
        std::filesystem::remove(sceneFileDirectory + "/headers.json");

        // Writes all of the chunks
        for(size_t i = 0; i < scene.chunks.size(); i++){
            writeChunkToDisk(sceneFileDirectory, scene.chunks[i]);
        }

        core::info("Function: writeSceneToDisk. Scene written to disk");
    }

    Chunk loadChunkFromDisk(std::string sceneFileDirectory, ChunkHeader chunk) {
        uint32_t chunkID = chunk.chunkID;
        std::cout << "[loadChunkFromDisk] Loading chunk " << chunkID << " from disk...";
        core::info("Function: loadChunkFromDisk. Loading chunk " + std::to_string(chunkID) + "from disk...");
        auto start = std::chrono::high_resolution_clock::now();

        // Find the header for the inputted chunkID.
        Chunk chunkData;
        std::vector<ChunkHeader> chunkHeaders = readHeadersJSON(sceneFileDirectory + "/headers.json");
        auto it = std::find_if(chunkHeaders.begin(), chunkHeaders.end(), [chunkID](const ChunkHeader& header) {
            return header.chunkID == chunkID;
        });

        // If the header is found, Assign it to the chunkData.
        if (it != chunkHeaders.end()) {
            chunkData.header = *it;
        } else {
            std::cerr << "[loadChunkFromDisk] Chunk ID " << chunkID << " not found in headers." << std::endl;
            core::error("Function: loadChunkFromDisk. ChunkID " + std::to_string(chunkID) + " not found in headers.");
            return chunkData; // Return empty chunkData
        }

        // Read the octree and voxelTypeData from disk.
        chunkData.geometryData = readUint32Vector(sceneFileDirectory + "/octree/" + std::to_string(chunkData.header.chunkID) + ".bin");
        chunkData.voxelTypeData = readUint32Vector(sceneFileDirectory + "/voxelTypeData/" + std::to_string(chunkData.header.chunkID) + ".bin");

        chunkData.LOD = 0;

        auto end = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double, std::milli>(end - start).count();
        core::info("Function: loadChunkFromDisk. Loaded chunk " + std::to_string(chunkID) + "in " + std::to_string(elapsed) + "ms");
        return chunkData;
    }

    void writeChunkToDisk(std::string sceneFileDirectory, Chunk chunk) {
        // Reads the header data if it exists.
        std::vector<ChunkHeader> chunkHeaders;
        std::ifstream inFile(sceneFileDirectory + "/headers.json");
        if (inFile.peek() != std::ifstream::traits_type::eof()) { 
            chunkHeaders = readHeadersJSON(sceneFileDirectory + "/headers.json");
        }
        inFile.close();

        // Iterates over all headers and finds the matching one, if there is none it flags that.
        bool headerFound = false;
        size_t headerIndex = 0;
        for(; headerIndex < chunkHeaders.size(); headerIndex++){ 
            if(chunkHeaders[headerIndex].chunkID == chunk.header.chunkID){
                chunkHeaders[headerIndex] = chunk.header;
                headerFound = true;
                break;
            }
        }

        // If the header isn't found then we add it to the list, otherwise we replace the old one.
        if(!headerFound){ 
            chunkHeaders.push_back(chunk.header);
        } else {
            chunkHeaders[headerIndex] = chunk.header;
        }

        // Writes our octree and voxelTypeData and creates it if it exists. Also writes headers.
        writeHeadersJSON(chunkHeaders, sceneFileDirectory + "/headers.json");
        writeUint32Vector(chunk.geometryData, sceneFileDirectory + "/octree/" + std::to_string(chunk.header.chunkID) + ".bin");
        writeUint32Vector(chunk.voxelTypeData, sceneFileDirectory + "/voxelTypeData/" + std::to_string(chunk.header.chunkID) + ".bin");
    }

    Scene loadSceneFromDisk(std::string sceneFileDirectory) {
        Scene scene;

        // Loads our chunk headers into memory.
        std::vector<ChunkHeader> chunkHeaders = readHeadersJSON(sceneFileDirectory + "/headers.json");

        // Loops over all of the chunk headers and loads the corresponding octree and voxelTypeData.
        for(size_t i = 0; i < chunkHeaders.size(); i++){ 
            Chunk chunk = loadChunkFromDisk(sceneFileDirectory, chunkHeaders[i]);
            scene.chunks.push_back(chunk);
        }
        return scene;
    }

    std::vector<ChunkHeader> loadChunkHeadersFromDisk(std::string sceneFileDirectory) {
        std::vector<ChunkHeader> sceneHeaders;
        std::vector<ChunkHeader> chunkHeaders = readHeadersJSON(sceneFileDirectory + "/headers.json");
        return sceneHeaders;
    }

    std::vector<ChunkHeader> getChunkHeadersFromScene(Scene& scene) {
        std::vector<ChunkHeader> sceneHeaders;
        for(size_t i = 0; i < scene.chunks.size(); i++) {
            ChunkHeader chunkHeader = scene.chunks[i].header;
            sceneHeaders.emplace_back(chunkHeader);
        }
        return sceneHeaders;
    }
}
