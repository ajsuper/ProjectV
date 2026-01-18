#ifndef SHADER_H
#define SHADER_H

#include <vector>
#include <string>

namespace projv {
    struct Shader {
        uint shaderID;
        std::string filePath;
        std::vector<char> shaderFileContents;
    };
}

#endif
