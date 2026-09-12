#ifndef NEWTON_PHYSICS_WORLD_H
#define NEWTON_PHYSICS_WORLD_H

#include <memory>

#include "physics/physicsWorld.h"

// The Newton backend's only entry point. Everything else about it - the
// embedded CPython interpreter, pybind11, the aviator_newton bridge module -
// lives inside newtonPhysicsWorld.cpp, which is why this header pulls in
// nothing but the public physics API: the rest of the engine must not grow a
// dependency on Python headers just because physics happens to use them
// (docs/newton_physics.md, rule 3).
//
// Compiled into the app only when AVIATOR_WITH_NEWTON is defined; the factory
// in physicsWorld.cpp guards the call with the same macro.
//
// Returns null - never throws - when Newton cannot start: no Python
// environment, newton/warp not installed, a Python error while building the
// bridge World. The reason is logged with SDL_Log before returning, so the
// caller only has to fall back to the Null backend.
std::unique_ptr<IPhysicsWorld> CreateNewtonPhysicsWorld(const PhysicsSettings &settings);

#endif
