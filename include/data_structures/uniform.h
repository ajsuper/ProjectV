#ifndef UNIFORM_H
#define UNIFORM_H

#include <string>

enum UniformType {Vec4, Mat3x3, Mat4x4};

struct Uniform {
    unsigned int uniformID;
    UniformType type;
    std::string name;
};

#endif
