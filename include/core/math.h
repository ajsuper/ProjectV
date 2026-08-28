#ifndef PROJV_CORE_MATH_H
#define PROJV_CORE_MATH_H
// This is a wrapper for the glm library, to provide consistency accross the engine.

// bx requires BX_CONFIG_DEBUG to be defined before any of its headers are included, but this
// must not overrule a value the build already set: bgfx.cmake defines it per configuration, and
// defining it again here both warns and silently forced bx's debug config -- assertions and all
// -- on every release build that included this header. Fall back to deriving it from NDEBUG so a
// consumer that sets nothing still gets the sensible thing.
#ifndef BX_CONFIG_DEBUG
#  ifdef NDEBUG
#    define BX_CONFIG_DEBUG 0
#  else
#    define BX_CONFIG_DEBUG 1
#  endif
#endif

#include "bx/math.h"
#include "bx/bx.h"
#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"
#include "glm/gtc/type_ptr.hpp"

namespace projv::core {
    using vec1 = glm::vec1;
    using vec2 = glm::vec2;
    using vec3 = glm::vec3;
    using vec4 = glm::vec4;

    using ivec1 = glm::ivec1;
    using ivec2 = glm::ivec2;
    using ivec3 = glm::ivec3;
    using ivec4 = glm::ivec4;

    using dvec1 = glm::dvec1;
    using dvec2 = glm::dvec2;
    using dvec3 = glm::dvec3;
    using dvec4 = glm::dvec4;

    using mat2 = glm::mat2;
    using mat3 = glm::mat3;
    using mat4 = glm::mat4;

    using quat = glm::quat;

    using glm::normalize;
    using glm::cos;
    using glm::acos;
    using glm::sin;
    using glm::asin;
    using glm::tan;
    using glm::atan;
    using glm::radians;
    using glm::degrees;

    using glm::dot;
    using glm::cross;
    using glm::length;
    using glm::distance;

    using glm::transpose;
    using glm::inverse;

    using glm::mix;
    using glm::clamp;
    using glm::step;
    using glm::smoothstep;
    using glm::min;
    using glm::max;

    using glm::fract;
    using glm::sign;
    using glm::pow;
    using glm::log;
    using glm::log2;

    using glm::lookAt;
    inline mat4 projectionMatrix(float fovy, float aspect, float nearPlane, float farPlane, bool homogeneousDepth) {
        float out[16];
        bx::mtxProj(out, fovy, aspect, nearPlane, farPlane, homogeneousDepth);
        return glm::make_mat4(out);
    }
    
    /**
     * A simple spline function that takes in an input that is modified by the points vector.
     * 
     * @param input The input to be modified.
     * @param points The vector of points that outline the spline that the input will follow.
     * 
     * @return Returns the modified point.
     */
    float evaluateCurve(float input, std::vector<vec2> points);
}

#endif
