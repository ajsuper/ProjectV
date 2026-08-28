#ifndef PROJV_SHADER_H
#define PROJV_SHADER_H

#include <cstdint>
#include <vector>
#include <string>

namespace projv {
    struct Shader {
        uint32_t shaderID;
        std::string filePath;
        std::vector<char> shaderFileContents;
    };
}

#endif
