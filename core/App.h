#pragma once
/*
 * App.h
 * -----
 * Top-level application
 *
 * Owns the three engine pillars:
 *   s_Renderer — SDL_GPU graphics system
 *   s_World    — ECS registry + Jolt physics simulation
 *   s_Time     — frame-timing resource
 *   s_Input    — per-frame input snapshot
 *
 * All methods are static (one-per-process design).  The four SDL3 callbacks
 * in main.cpp delegate directly to these methods:
 *
 *   SDL_AppInit()    → App::Initialize()   — creates window, GPU device, spawns scene
 *   SDL_AppIterate() → App::RunFrame()     — tick time → update world → render
 *   SDL_AppEvent()   → App::ProcessInput() — translate SDL events → InputState
 *   SDL_AppQuit()    → App::Shutdown()     — release all GPU / physics resources
 *
 * To add a new startup system:  call your spawn function inside Initialize().
 * To add a new per-frame system: call it inside RunFrame() before/after World::Update().
 */

#include <SDL3/SDL.h>
#include <array>
#include <string>

#include "renderer/Renderer.h"
#include "ecs/World.h"
#include "core/Input.h"
#include "core/Time.h"

//Time Resource
struct Time{
    float deltaTime = 0.0f;
    float elapsed = 0.0f;
    float lastFrame = 0.0f;
}

class App
{
public:
    static SDL_AppResult Initialize();
    static SDL_AppResult RunFrame();
    static SDL_AppResult ProcessInput(SDL_Event* event);
    static void          Shutdown();

    // Default window resolution — passed to Renderer::Init().
    static constexpr float kWindowWidth  = 1500.0f;
    static constexpr float kWindowHeight =  900.0f;

private:
    static Renderer   s_Renderer;
    static World      s_World;
    static InputState s_Input;
    static Time       s_Time;
    static World    
};