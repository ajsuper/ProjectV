#ifndef PROJV_UNIFORM_H
#define PROJV_UNIFORM_H
#include <string>

namespace projv {
    enum UniformType {Vec4, Mat3x3, Mat4x4};

    struct Uniform {
        unsigned int uniformID;
        UniformType type;
        std::string name;
    };
}

#endif
