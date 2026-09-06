#ifndef SCENE_BUILDERS_H
#define SCENE_BUILDERS_H

#include <entt/entt.hpp>

#include "scene/scene.h"

// Scene builders. Each one authors its own geometry, textures, camera framing
// and sky, so swapping scenes is a one-line change in main().
//
// They write the camera entity's CameraComponent directly, which is why the
// renderer must read sky/horizon/depth *after* the builder has run - see
// Renderer::SyncSceneSettings.

void BuildFinalScene(Scene &scene, entt::registry &registry);
void BuildTestAllFeatureScene(Scene &scene, entt::registry &registry);

#endif
