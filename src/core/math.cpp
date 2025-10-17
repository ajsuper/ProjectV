#include "core/math.h"

namespace projv::core {
    float evaluateCurve(float input, std::vector<vec2> points) {
        // Edge cases
        if (input <= points.front().x)
            return points.front().y;
        if (input >= points.back().x)
            return points.back().y;

        // Find the two points input lies between.
        for (size_t i = 0; i < points.size() - 1; ++i) {
            const vec2& p0 = points[i];
            const vec2& p1 = points[i + 1];
            if (input >= p0.x && input <= p1.x) {
                float t = (input - p0.x) / (p1.x - p0.x); // normalize [0,1]    
                return p0.y + t * (p1.y - p0.y); // linear interpolation
            }
        }
    
        return 0.0f; // fallback.
    }
}
