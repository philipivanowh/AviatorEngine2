#ifndef CAMERA_H
#define CAMERA_H

#define LSHIFT_KEY 225
#define W_KEY 26
#define A_KEY 4
#define S_KEY 22
#define D_KEY 7
#define Q_KEY 20
#define E_KEY 8 
#define SPACE_KEY 44
#define LCTRL_KEY 224

#include "common.h"

class Camera
{
public:
    point3 position;
    point3 target{0.0f, 0.0f, 0.0f};
    float yaw;   // radians, rotation around world Y
    float pitch; // radians, clamped to +-PITCH_LIMIT
    float focus_dist;
    float fov;
    float defocus_angle;
    Color sky{0.0f, 0.0f, 0.0f};
    Color horizon{0.0f, 0.0f, 0.0f};

    Camera(Vec3<float> position,float yaw,float pitch){
        this->position = position;
        this->yaw = yaw;
        this->pitch = pitch;
        this->focus_dist = 10.0f;
        this->fov = 40.0f;
        this->defocus_angle = 0.2f;
    }

    Vec3<float> Forward()
    {
        return Vec3<float>(std::cosf(pitch) * std::cosf(yaw), std::sinf(pitch), std::cosf(pitch) * std::sinf(yaw));
    }

    void Update(const bool *keys, float mouseDeltaX, float mouseDeltaY, float dt)
    {
        UpdateLook(mouseDeltaX, mouseDeltaY);
        UpdateMove(keys, dt);
    }

    void UpdateLook(float mouseDeltaX, float mouseDeltaY)
    {
        yaw += mouseDeltaX * MOUSE_SENSITIVITY;
        pitch -= mouseDeltaY * MOUSE_SENSITIVITY;
        pitch = clamp(pitch, -PITCH_LIMIT, PITCH_LIMIT);
    }

    void UpdateMove(const bool *keys, float dt)
    {
        const Vec3<float> forward = Forward();
        const Vec3<float> right = normalize(cross(forward, Vec3<float>{0.0f, 1.0f, 0.0f}));

        float speed = CAMERA_MOVE_SPEED * dt;
        if (keys[LSHIFT_KEY])
        {
            speed *= CAMERA_SPRINT_MULTIPLIER;
        }

        if (keys[W_KEY])
            this->position = this->position + forward * speed;
        if (keys[S_KEY])
            this->position = this->position - forward * speed;
        if (keys[D_KEY])
            this->position = this->position + right * speed;
        if (keys[A_KEY])
            this->position = this->position - right * speed;
        if (keys[SPACE_KEY])
            this->position.y += speed;
        if (keys[LCTRL_KEY])
            this->position.y -= speed;
        if (keys[Q_KEY])
            this->defocus_angle -= 0.1;
        if (keys[E_KEY])
            this->defocus_angle += 0.1;
        this->defocus_angle = clamp(this->defocus_angle, 0.0f,1.0f);
    }

private:
    const float CAMERA_MOVE_SPEED = 40.0f; // units/sec at normal (non-sprint) speed
    const float CAMERA_SPRINT_MULTIPLIER = 3.0f;
    const float MOUSE_SENSITIVITY = 0.0025f; // radians of turn per pixel of mouse delta
    const float PITCH_LIMIT = 1.5533f;       // ~89 degrees; stops the camera flipping over at the poles
};
#endif