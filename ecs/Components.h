#pragma once
#include <string>

#include <SDL3/SDL.h>
#include "common.h"

struct InputState{

}

struct TagComponent {
    std::string Tag;
};

struct TransformComponent {
 
};

struct MeshComponent {
 
};

struct PhysicsComponent {
    bool IsStatic = false;
};

struct LightComponent {
    Color color = Color(1.0f);
    float     emission = 1.0f;
};

struct CameraComponent {
    Camera cameraInstance;
};

// ── Light components ───────────────────────────────────────────────────────────

// ── Material component ─────────────────────────────────────────────────────────
struct MaterialComponent {
    // Empty string = no texture; renderer binds the 1×1 white fallback instead.
    std::string diffusePath  = "";
    std::string specularPath = "";
    float       shininess    = 32.0f;

    // Set dirty = true after changing a path to trigger a reload next frame.
    bool dirty = true;

    // Populated by Renderer — treat as read-only from game code.
    SDL_GPUTexture *diffuseTexture  = nullptr;
    SDL_GPUTexture *specularTexture = nullptr;
};