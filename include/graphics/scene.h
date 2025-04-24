#ifndef PROJECTV_SCENE_H
#define PROJECTV_SCENE_H

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <math.h>
#include <cstring>
#include <omp.h>

#include "data_structures/scene.h"
#include "core/log.h"

namespace projv::graphics {
    /**
     * Passes the scene to the fragment shader, very costly as it updates entire scene every time it is sent.
     * 
     * @param sceneToRender The scene to be passed to the fragment shader.
     */
    void passSceneToOpenGL(Scene& sceneToRender);
}

#endif