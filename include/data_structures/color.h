#ifndef COLOR_H
#define COLOR_H

#include <stdint.h>

namespace projv{
    /**
     * @struct Color
     * @brief An RGB value with all channels from 0.0-1.0
     */
    struct Color {
        uint8_t r;
        uint8_t g;
        uint8_t b;
    };
}

#endif