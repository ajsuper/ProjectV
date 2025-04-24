#ifndef PROJV_CORE_MATH_H
#define PROJV_CORE_MATH_H
// This is a wrapper for the glm library, to provide consistency accross the engine.
#include "dependencies/glm/glm/glm.hpp"

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