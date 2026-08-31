#pragma once

#include <math/vec2.h>

struct InputState
{
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool shift = false;
    Vec2<float> mouseOffset(0.0f, 0.0f);
};