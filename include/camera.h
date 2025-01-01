#ifndef CAMERA_H
#define CAMERA_H

namespace projv{
    /**
     * @struct Camera
     * A structure to hold the camera's position, direction, up vector, and movement/rotation speeds.
     */
    struct Camera {
        float position[3] = {0.0, 0.0, 0.0};
        float direction[3] = {1.0, 0, 0.0};
        float up[3] = { 0, 1, 0 };
        float movementSpeed = 1.0f;
        float rotationSpeed = 0.01f;
    };

    // Global camera to be used throughout the project.
    inline Camera cam;
}

#endif
