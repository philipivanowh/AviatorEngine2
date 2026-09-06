#ifndef CAMERA_SYSTEM_H
#define CAMERA_SYSTEM_H

#include "core/camera.h"
#include "math/vec3.h"
#include <entt/entt.hpp>
#include "scene/components.h"

// --- Camera setup: derive an initial yaw/pitch that reproduces the scene's
// authored framing, then hand control over to mouse-look + WASD from here on.
inline void CameraInitialize(entt::registry &m_Register)
{

    auto view = m_Register.view<CameraComponent, TransformComponent>();

    if (view)
    {
        entt::entity cam_entity = view.front();
        auto &transform = view.get<TransformComponent>(cam_entity);
        auto &cam = view.get<CameraComponent>(cam_entity);

        const Vec3<float> initialForward =
            normalize(cam.target - transform.position);
        cam.yaw = SDL_atan2f(cam.target.z - transform.position.z, cam.target.x - transform.position.x);

        cam.pitch = SDL_asinf(initialForward.y);
    }
}

inline Vec3<float> Forward(entt::registry &m_Register)
{
    auto view = m_Register.view<CameraComponent, TransformComponent>();

    if (view)
    {
        entt::entity cam_entity = view.front();
        auto &cam = view.get<CameraComponent>(cam_entity);

        return Vec3<float>(std::cosf(cam.pitch) * std::cosf(cam.yaw), std::sinf(cam.pitch), std::cosf(cam.pitch) * std::sinf(cam.yaw));
    }
    // return Vec3<float>(std::cosf(pitch) * std::cosf(yaw), std::sinf(pitch), std::cosf(pitch) * std::sinf(yaw));
    return Vec3<float>(0.0f, 0.0f, -1.0f); // Default forward direction
}

inline Vec3<float> Forward(const CameraComponent &cam)
{
    return Vec3<float>(std::cosf(cam.pitch) * std::cosf(cam.yaw), std::sinf(cam.pitch), std::cosf(cam.pitch) * std::sinf(cam.yaw));
}

inline void UpdateLook(CameraComponent &cam, float mouseDeltaX, float mouseDeltaY)
{
    cam.yaw += mouseDeltaX * cam.MOUSE_SENSITIVITY;
    cam.pitch -= mouseDeltaY * cam.MOUSE_SENSITIVITY;
    cam.pitch = clamp(cam.pitch, -cam.PITCH_LIMIT, cam.PITCH_LIMIT);
}

inline void UpdateMove(TransformComponent &transform, CameraComponent &cam, const bool *keys, float dt)
{
    const Vec3<float> forward = Forward(cam);
    const Vec3<float> right = normalize(cross(forward, Vec3<float>{0.0f, 1.0f, 0.0f}));

    float speed = cam.CAMERA_MOVE_SPEED * dt;
    if (keys[LSHIFT_KEY])
    {
        speed *= cam.CAMERA_SPRINT_MULTIPLIER;
    }

    if (keys[W_KEY])
        transform.position = transform.position + forward * speed;
    if (keys[S_KEY])
        transform.position = transform.position - forward * speed;
    if (keys[D_KEY])
        transform.position = transform.position + right * speed;
    if (keys[A_KEY])
        transform.position = transform.position - right * speed;
    if (keys[SPACE_KEY])
        transform.position.y += speed;
    if (keys[LCTRL_KEY])
        transform.position.y -= speed;
    if (keys[Q_KEY])
        cam.defocus_angle -= 0.1;
    if (keys[E_KEY])
        cam.defocus_angle += 0.1;
    cam.defocus_angle = clamp(cam.defocus_angle, 0.0f, 1.0f);
}

inline void UpdateCamera(entt::registry &m_Register, const bool *keys, float mouseDeltaX, float mouseDeltaY, float dt)
{
    auto view = m_Register.view<CameraComponent, TransformComponent>();
   
    if (view)
    {
        entt::entity cam_entity = view.front();
        auto &transform = view.get<TransformComponent>(cam_entity);
        auto &cam = view.get<CameraComponent>(cam_entity);

        UpdateLook(cam, mouseDeltaX, mouseDeltaY);
        UpdateMove(transform, cam, keys, dt);
    }
}

#endif