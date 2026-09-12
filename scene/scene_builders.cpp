#include "scene/scene_builders.h"

#include <SDL3/SDL.h>

#include "core/common.h"
#include "scene/components.h"
#include "scene/material.h"

// The classic Cornell box: two coloured side walls, three white ones, a ceiling
// light, and two boxes. Lit entirely by the ceiling light against a black sky.
// void BuildCornellBox(Scene &scene)
// {
//     scene.camera.position = Point3(278.0f, 278.0f, -800.0f);
//     scene.camera.target = Point3(278.0f, 278.0f, 0.0f);
//     scene.camera.fov = 40.0f;
//     scene.camera.focus_dist = 800.0f;
//     scene.camera.sky = Color(0.0f, 0.0f, 0.0f);
//     scene.camera.horizon = Color(0.0f, 0.0f, 0.0f);
//     scene.maxDepth = 2;

//     const Material white = Material::Lambertian(Color(0.73f, 0.73f, 0.73f));
//     const Material red = Material::Lambertian(Color(0.65f, 0.05f, 0.05f));
//     const Material green = Material::Lambertian(Color(0.12f, 0.45f, 0.15f));
//     const Material light = Material::Emissive(Color(1.0f, 1.0f, 1.0f), 65.0f);
//     const Material redLight = Material::Emissive(Color(0.65f, 0.65f, 0.65f), 10.0f);
//     const Material volume = Material::Volume(Color(0.22f, 0.22f, 0.1f), 0.3f);

//     // Walls
//     scene.AddQuad(Point3(555.0f, 0.0f, 0.0f), Vec3(0.0f, 555.0f, 0.0f), Vec3(0.0f, 0.0f, 555.0f), green);       // left
//     scene.AddQuad(Point3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 555.0f, 0.0f), Vec3(0.0f, 0.0f, 555.0f), red);           // right
//     scene.AddQuad(Point3(0.0f, 0.0f, 0.0f), Vec3(555.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 555.0f), white);         // floor
//     scene.AddQuad(Point3(555.0f, 555.0f, 555.0f), Vec3(-555.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, -555.0f), white); // ceiling
//     scene.AddQuad(Point3(0.0f, 0.0f, 555.0f), Vec3(555.0f, 0.0f, 0.0f), Vec3(0.0f, 555.0f, 0.0f), white);       // back

//     // Ceiling light, just below the ceiling so it isn't coplanar with it.
//     scene.AddQuad(Point3(343.0f, 554.0f, 332.0f), Vec3(-130.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, -105.0f), light);

//     scene.AddSphere(Point3(350.0f, 150.0f, 50.0f), 50.0f, redLight);

//     // Both boxes are built axis-aligned at the origin corner, then rotated as
//     // rigid bodies and translated into place - the canonical Cornell framing.
//     scene.AddBox(Point3(0.0f, 0.0f, 0.0f), Point3(165.0f, 330.0f, 165.0f), volume)
//         .RotateY(static_cast<float>(degrees_to_radians(15.0)))
//         .Translate(Vec3(265.0f, 0.0f, 295.0f));

//     scene.AddBox(Point3(0.0f, 0.0f, 0.0f), Point3(165.0f, 165.0f, 165.0f), volume)
//         .RotateY(static_cast<float>(degrees_to_radians(-18.0)))
//         .Translate(Vec3(130.0f, 0.0f, 65.0f));
// }

// A deliberately small scene for reading what `density` actually does. Three
// identical fog spheres sit in a row over a lit floor, an order of magnitude
// apart in density, with a solid red sphere buried in each so you can see how
// far into the medium you can still see.
//
// Density is per world unit: the chance of crossing distance d without
// scattering is exp(-density * d), so mean free path is 1/density. At the 2.0
// radius here that's 10 units for the thin one (barely visible), 1 unit for the
// middle one (smoke), and 0.1 for the thick one (nearly opaque).
// void BuildFogTest(Scene &scene)
// {
//     scene.camera.position = Point3(0.0f, 3.0f, -14.0f);
//     scene.camera.target = Point3(0.0f, 1.0f, 0.0f);
//     scene.camera.fov = 40.0f;
//     scene.camera.focus_dist = 14.0f;
//     scene.camera.sky = Color(0.02f, 0.03f, 0.05f);
//     scene.camera.horizon = Color(0.01f, 0.01f, 0.02f);
//     scene.maxDepth = 40; // dense fog: many scatters before a path finds the light

//     const Material floor = Material::Lambertian(Color(0.6f, 0.6f, 0.6f));
//     const Material marker = Material::Lambertian(Color(0.9f, 0.1f, 0.1f));

//     scene.AddQuad(Point3(-20.0f, 0.0f, -20.0f),
//                   Vec3(40.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 40.0f), floor);

//     // Two overhead lights, so the fog is side-lit and its depth reads.
//     scene.AddQuad(Point3(-6.0f, 9.0f, -3.0f),
//                   Vec3(12.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 6.0f),
//                   Material::Emissive(Color(1.0f, 0.95f, 0.9f), 6.0f));

//     const float densities[3] = {0.1f, 1.0f, 10.0f};
//     for (int i = 0; i < 3; i++)
//     {
//         const float x = (i - 1) * 5.5f;

//         // The marker sits at the centre of the fog ball.
//         scene.AddSphere(Point3(x, 2.0f, 0.0f), 0.6f, marker);
//         scene.AddVolume(Point3(x, 2.0f, 0.0f), 2.0f, Color(0.9f, 0.9f, 0.95f), densities[i]);
//     }

//     // A fog-filled box on the right, to check the Box boundary path as well as
//     // the sphere one - they take different span code in the shader.
//     scene.AddSphere(Point3(9.0f, 1.2f, 3.0f), 0.6f, marker);
//     scene.AddVolume(Point3(7.0f, 0.0f, 1.0f), Point3(11.0f, 3.0f, 5.0f),
//                     Color(0.35f, 0.55f, 0.95f), 1.0f)
//         .RotateY(static_cast<float>(degrees_to_radians(20.0)));

//     // A solid box on the left for reference, same size, same primitive.
//     scene.AddBox(Point3(-11.0f, 0.0f, 1.0f), Point3(-7.0f, 3.0f, 5.0f),
//                  Material::Lambertian(Color(0.3f, 0.7f, 0.4f)))
//         .RotateY(static_cast<float>(degrees_to_radians(-20.0)));
// }

// Ray Tracing In One Weekend's "final scene": a big ground sphere, three hero
// spheres showing off each material type, and a field of small random ones.
// Lit by a blue sky gradient rather than by any emissive geometry.
void BuildFinalScene(Scene &scene, entt::registry &registry)
{

    auto view = registry.view<CameraComponent, TransformComponent>();

    if (!view)
    {
        SDL_Log("No camera entity found in registry.");
        return;
    }

    entt::entity cam_entity = view.front();
    auto &transform = view.get<TransformComponent>(cam_entity);
    auto &cam = view.get<CameraComponent>(cam_entity);

    transform.position = Point3(13.0f, 2.0f, 3.0f);
    cam.target = Point3(0.0f, 0.0f, 0.0f);
    cam.fov = 20.0f;
    cam.focus_dist = 10.0f;
    cam.defocus_angle = 0.6f;
    cam.sky = Color(0.5f, 0.7f, 1.0f);
    cam.horizon = Color(1.0f, 1.0f, 1.0f);

    scene.maxDepth = 3;

    const Texture *brick = scene.textures.Load("brick/textures/red_brick_diff_4k.jpg");

    scene.AddSphere(registry, Point3(10.0f, 10.0f, 0.0f), 5.0f, Material::Emissive(Color(0.94f, 0.94f, 0.94f), 5.0f));

    // Analytic on purpose. As a 32x16 mesh a radius-1000 sphere has facets
    // about 200 units across, so the "ground" would be a shallow cone and the
    // small spheres below would float above it.
    scene.AddAnalyticSphere(registry,
                            Point3(0.0f, -1000.0f, 0.0f), 1000.0f,
                            Material::Lambertian(Color(0.5f, 0.5f, 0.5f)));

    // The three heroes: glass, matte (brick-textured), and metal.
    scene.AddSphere(registry, Point3(0.0f, 1.0f, 0.0f), 1.0f, Material::Dielectric(1.5f));

    // Rotating a sphere can't change its shape, so RotateY only turns the
    // texture on it - which is exactly what you want for aiming a brick seam.
    scene.AddSphere(registry,
                    Point3(-4.0f, 1.0f, 0.0f), 1.0f,
                    Material::Lambertian(Color(1.0f, 1.0f, 1.0f)).Textured(brick))
        .RotateY(static_cast<float>(degrees_to_radians(90.0)));
    scene.AddSphere(registry, Point3(4.0f, 1.0f, 0.0f), 1.0f, Material::Metal(Color(0.7f, 0.6f, 0.5f)));

    SDL_srand(0);
    for (int a = -5; a < 5; a++)
    {
        for (int b = -5; b < 5; b++)
        {
            const Point3 position(a + 0.9f * SDL_randf(), 0.2f, b + 0.9f * SDL_randf());
            const float roll = SDL_randf();

            Material mat;
            if (roll < 0.8f)
            {
                mat = Material::Lambertian(Color(SDL_randf(), SDL_randf(), SDL_randf()));
            }
            else if (roll < 0.95f)
            {
                mat = Material::Metal(
                    Color(0.5f + 0.5f * SDL_randf(), 0.5f + 0.5f * SDL_randf(), 0.5f + 0.5f * SDL_randf()),
                    SDL_randf() * 0.2f);
            }
            else
            {
                mat = Material::Dielectric(1.5f);
            }

            scene.AddSphere(registry, position, 0.2f, mat);
        }
    }
}

// A rigid-body playground for the physics backend. Everything in it is authored
// through the Scene::Add*Physics* helpers, so each object is a render shape plus
// physics components created together; IPhysicsWorld::Build picks them up from
// the registry afterwards (docs/newton_physics.md).
//
// Scale is SI - one unit is one metre, gravity is -9.81 along Y - because that
// is what Newton's solvers are tuned for. A 1 m box settling at 60 Hz is the
// case XPBD is built around; a scene authored "about 500 units tall" would need
// every stiffness and threshold retuned.
void BuildPhysicsScene(Scene &scene, entt::registry &registry)
{
    auto view = registry.view<CameraComponent, TransformComponent>();

    if (!view)
    {
        SDL_Log("No camera entity found in registry.");
        return;
    }

    entt::entity cam_entity = view.front();
    auto &transform = view.get<TransformComponent>(cam_entity);
    auto &cam = view.get<CameraComponent>(cam_entity);

    transform.position = Point3(-3.0f, 9.0f, 26.0f);
    cam.target = Point3(0.0f, 2.0f, 0.0f);
    cam.fov = 100.0f;
    cam.focus_dist = 26.0f;
    cam.defocus_angle = 0.0f; // everything here is worth seeing sharply
    cam.sky = Color(0.45f, 0.62f, 0.95f);
    cam.horizon = Color(0.95f, 0.96f, 1.0f);
    cam.CAMERA_MOVE_SPEED = 50.0f;

    scene.maxDepth = 3;

    const Material concrete = Material::Lambertian(Color(0.62f, 0.62f, 0.60f));
    const Material red = Material::Lambertian(Color(0.72f, 0.20f, 0.18f));
    const Material cream = Material::Lambertian(Color(0.83f, 0.78f, 0.66f));
    const Material teal = Material::Lambertian(Color(0.16f, 0.52f, 0.52f));
    const Material timber = Material::Lambertian(Color(0.55f, 0.38f, 0.22f));
    const Material rubber = Material::Lambertian(Color(0.10f, 0.10f, 0.12f));
    const Material steel = Material::Metal(Color(0.78f, 0.78f, 0.82f), 0.08f);
    const Material chrome = Material::Metal(Color(0.92f, 0.92f, 0.95f));
    const Material glass = Material::Dielectric(1.5f);

    // --- Ground, and a sun to light it. The light is analytic and static on
    // purpose: emissive geometry is what the shaders sample directly, and
    // nothing in the solver should ever move it.
    scene.AddPhysicsGround(registry, 0.0f, 40.0f, concrete, PhysicsSurface{0.9f, 0.1f});
    scene.AddSphere(registry, Point3(14.0f, 22.0f, 10.0f), 4.0f,
                    Material::Emissive(Color(1.0f, 0.96f, 0.9f), 4.0f));

    // --- A five-row pyramid of 1 m boxes: the classic stacking test. Friction
    // is what holds it together, so it is deliberately high.
    const float cube = 0.5f;
    for (int row = 0; row < 5; row++)
    {
        const int count = 5 - row;
        for (int i = 0; i < count; i++)
        {
            const float x = (i - 0.5f * (count - 1)) * (2.0f * cube + 0.02f);
            scene.AddPhysicsBox(registry,
                                Point3(x, cube + row * 2.0f * cube, -4.0f),
                                Vec3<float>(cube, cube, cube),
                                (row + i) % 2 == 0 ? cream : red,
                                RigidbodyComponent{.mass = 1.0f},
                                PhysicsSurface{0.9f, 0.0f});
        }
    }

    // --- A tower, each box yawed a little further than the one below. Yaw is
    // the one rotation an analytic box could have drawn; these are mesh boxes
    // like every physics box, so they can also topple properly.
    for (int i = 0; i < 8; i++)
    {
        scene.AddPhysicsBox(registry, Point3(-9.0f, 0.4f + i * 0.8f, 2.0f),
                            Vec3<float>(0.6f, 0.4f, 0.6f), i % 2 == 0 ? teal : cream,
                            RigidbodyComponent{.mass = 2.0f}, PhysicsSurface{0.8f, 0.0f})
            .Rotate(QuatY(static_cast<float>(degrees_to_radians(9.0 * i))));
    }

    // --- Dominoes. The first one is nudged with angular velocity rather than a
    // force, so the chain starts the instant the simulation does: spinning about
    // -Z tips its top toward +X, into its neighbour.
    for (int i = 0; i < 14; i++)
    {
        const Vec3<float> nudge =
            i == 0 ? Vec3<float>(0.0f, 0.0f, -3.5f) : Vec3<float>(0.0f, 0.0f, 0.0f);

        scene.AddPhysicsBox(registry, Point3(3.0f + i * 0.9f, 0.6f, 7.0f),
                            Vec3<float>(0.08f, 0.6f, 0.35f), i % 2 == 0 ? cream : teal,
                            RigidbodyComponent{.mass = 0.4f, .angularVelocity = nudge},
                            PhysicsSurface{0.7f, 0.0f});
    }

    // --- A ramp: a box rotated about Z, precisely the case an analytic box
    // cannot express, with balls to roll down it. Rotated by -18 degrees its
    // surface descends toward -X, so the balls start at the high end.
    scene.AddStaticBox(registry, Point3(10.0f, 2.0f, -1.0f), Vec3<float>(3.2f, 0.15f, 2.2f),
                       timber, PhysicsSurface{0.6f, 0.0f})
        .Rotate(QuatZ(static_cast<float>(degrees_to_radians(-18.0))));

    for (int i = 0; i < 3; i++)
    {
        scene.AddPhysicsSphere(registry, Point3(12.2f, 3.7f, -1.8f + i * 1.2f), 0.35f,
                               i == 1 ? chrome : steel, RigidbodyComponent{.mass = 1.5f},
                               PhysicsSurface{0.5f, 0.1f});
    }

    // --- Spheres dropped on the pyramid, one per material type, so the physics
    // scene doubles as a shading test. All analytic, which keeps the glass one's
    // refraction exact.
    scene.AddPhysicsSphere(registry, Point3(-1.0f, 8.0f, -4.0f), 0.5f, glass,
                           RigidbodyComponent{.mass = 3.0f}, PhysicsSurface{0.3f, 0.2f});
    scene.AddPhysicsSphere(registry, Point3(0.8f, 11.0f, -3.4f), 0.6f, chrome,
                           RigidbodyComponent{.mass = 6.0f}, PhysicsSurface{0.4f, 0.1f});
    scene.AddPhysicsSphere(registry, Point3(-0.4f, 14.0f, -4.6f), 0.45f, red,
                           RigidbodyComponent{.mass = 2.0f}, PhysicsSurface{0.6f, 0.0f});

    // --- A bouncy ball on its own, so restitution is visible with nothing else
    // in the way.
    scene.AddPhysicsSphere(registry, Point3(-5.0f, 10.0f, 6.0f), 0.5f, rubber,
                           RigidbodyComponent{.mass = 1.0f}, PhysicsSurface{0.6f, 0.95f});

    // --- The cannonball: heavy, fast, aimed at the pyramid. Its start velocity
    // lives on the component, so Reset relaunches it exactly the same way.
    const EntityGroup cannonball =
        scene.AddPhysicsSphere(registry, Point3(-17.0f, 1.2f, -4.0f), 0.7f, steel,
                               RigidbodyComponent{.mass = 45.0f,
                                                  .linearVelocity = Vec3<float>(19.0f, 2.2f, 0.0f)},
                               PhysicsSurface{0.4f, 0.1f});
    registry.get<TagComponent>(cannonball.Entity()).tag = "Cannonball";

    // --- A kinematic sweeper arm. Physics never writes a kinematic body's
    // transform: AnimateKinematicBodies spins this one every frame and the
    // solver pushes whatever is in its way (systems/physicsDemoSystem.h).
    const EntityGroup sweeper =
        scene.AddPhysicsBox(registry, Point3(0.0f, 0.45f, 12.0f), Vec3<float>(3.6f, 0.3f, 0.3f),
                            steel, RigidbodyComponent{.type = RigidbodyType::Kinematic},
                            PhysicsSurface{0.5f, 0.0f});
    registry.get<TagComponent>(sweeper.Entity()).tag = "Sweeper";

    for (int i = 0; i < 6; i++)
    {
        const float angle = static_cast<float>(degrees_to_radians(60.0 * i));
        scene.AddPhysicsSphere(registry,
                               Point3(2.6f * std::cos(angle), 0.3f, 12.0f + 2.6f * std::sin(angle)),
                               0.3f, i % 2 == 0 ? teal : cream, RigidbodyComponent{.mass = 0.5f},
                               PhysicsSurface{0.5f, 0.3f});
    }

    // --- A dumbbell: two weights and a bar welded into one rigid body. The
    // parts keep their own render shapes and follow the body every step.
    const size_t dumbbellFirst = scene.Count();
    scene.AddStaticSphere(registry, Point3(5.0f, 7.0f, 3.0f), 0.45f, steel, PhysicsSurface{0.6f, 0.0f});
    scene.AddStaticSphere(registry, Point3(6.8f, 7.0f, 3.0f), 0.45f, steel, PhysicsSurface{0.6f, 0.0f});
    scene.AddStaticBox(registry, Point3(5.9f, 7.0f, 3.0f), Vec3<float>(0.9f, 0.12f, 0.12f), chrome,
                       PhysicsSurface{0.6f, 0.0f});
    scene.MakeCompoundBody(registry, scene.GroupSince(registry, dumbbellFirst),
                           RigidbodyComponent{.mass = 14.0f});

    SDL_Log("Physics scene: %zu entities authored", scene.Count());
}


// Ray Tracing: The Next Week's final scene - the one that puts every feature in
// the book on screen at once. Two of its elements are what volumes were added
// for:
//
//   * the blue "subsurface" ball: a glass sphere with a dense medium inside it,
//     so light refracts in, scatters around, and refracts back out
//   * the global haze: a 5000-unit sphere of extremely thin white medium
//     wrapped around the whole scene, which is what softens the background
//
// Two substitutions from the book, since the engine has no procedural textures
// yet: the earth-mapped sphere uses the brick texture, and the Perlin-noise
// sphere is a plain matte one. Motion blur on the orange sphere is also
// skipped - Object_GPU carries a Position2 for it, but nothing on the CPU side
// sets it yet.
void BuildTestAllFeatureScene(Scene &scene, entt::registry &registry)
{
    auto view = registry.view<CameraComponent, TransformComponent>();

    if (!view)
    {
        SDL_Log("No camera entity found in registry.");
        return;
    }

    entt::entity cam_entity = view.front();
    auto &transform = view.get<TransformComponent>(cam_entity);
    auto &cam = view.get<CameraComponent>(cam_entity);
    transform.position = Point3(478.0f, 278.0f, -600.0f);
    cam.target = Point3(278.0f, 278.0f, 0.0f);
    cam.fov = 100.0f;
    cam.focus_dist = 600.0f;
    cam.sky = Color(0.0f, 0.0f, 0.0f);
    cam.horizon = Color(0.0f, 0.0f, 0.0f);
    cam.CAMERA_MOVE_SPEED = 40.0f;
    // Russian roulette (starting at bounce 1 in the shader) does the actual
    // termination work now, so this just needs to be a safe upper bound for
    // deep mirror/glass chains plus a few GI bounces - 30 was sized for the
    // old "always bounce to Depth" behavior and is unnecessary cost now.
    scene.maxDepth = 3;

    const Texture *brick = scene.textures.Load("brick/textures/red_brick_diff_4k.jpg");

    // --- Ground: a 20x20 grid of boxes at random heights.
    const Material ground = Material::Lambertian(Color(0.48f, 0.83f, 0.53f));
    SDL_srand(0);
    const int boxesPerSide = 20;
    for (int i = 0; i < boxesPerSide; i++)
    {
        for (int j = 0; j < boxesPerSide; j++)
        {
            const float w = 100.0f;
            const float x0 = -1000.0f + i * w;
            const float z0 = -1000.0f + j * w;
            const float y1 = 1.0f + SDL_randf() * 100.0f;
            // Analytic, not a mesh instance. The hybrid renderer rasterizes
            // plain boxes itself (IsRasterProxy in renderer.h), so the ground
            // still lands in the G-buffer - while bounce and shadow rays keep
            // hitting a one-slab-test box instead of walking a 12-triangle BLAS.
            scene.AddBox(registry, Point3(x0, 0.0f, z0), Point3(x0 + w, y1, z0 + w), ground);
        }
    }

    scene.AddTriangle(registry,
                      Point3(150.0f, 0.0f, 200.0f),
                      Point3(400.0f, 0.0f, 200.0f),
                      Point3(275.0f, 250.0f, 200.0f),
                      Material::Lambertian(Color(0.9f, 0.2f, 0.2f)).Textured(brick));

    // --- Mesh instances, the vertex-buffer path.
    //
    // These two are the correctness test for the two-level structure. The box
    // is 12 triangles with per-face normals, so it must look exactly like an
    // analytic AddBox of the same size - flat is the right answer for a box.
    // The sphere is where the payoff shows: HitMesh interpolates the per-vertex
    // normals CreateSphere has always computed, so it shades smooth rather than
    // faceted, and it costs one 48-byte instance instead of 2048 objects.
    const MeshHandle boxMesh = scene.meshes.Add(
        Mesh::CreateBox(Vec3<float>(60.0f, 60.0f, 60.0f),
                        Material::Lambertian(Color(1.0f, 1.0f, 1.0f)),
                        AABB{-60.0f, -60.0f, -60.0f, 60.0f, 60.0f, 60.0f}));

    const MeshHandle sphereMesh = scene.meshes.Add(
        Mesh::CreateSphere(10.0f, 32, 16, Material::Lambertian(Color(1.0f, 1.0f, 1.0f))));

    scene.AddMeshInstance(registry, boxMesh, Point3(-150.0f, 160.0f, 100.0f),
                          Material::Lambertian(Color(0.85f, 0.65f, 0.2f)));

    scene.AddMeshInstance(registry, sphereMesh, Point3(80.0f, 380.0f, 60.0f),
                          Material::Lambertian(Color(0.25f, 0.6f, 0.85f)));

    // An analytic twin of the mesh sphere: same radius, same material, offset
    // along the camera's horizontal axis so the two sit side by side. They
    // should be the same size and shade the same way - this is the regression
    // check for HitMesh against HitSphere, and it is what caught the inverted
    // triangle winding in CreateSphere.

    const MeshHandle sphereMesh1 = scene.meshes.Add(
        Mesh::CreateSphere(70.0f, 32, 16, Material::Lambertian(Color(1.0f, 1.0f, 1.0f))));

    scene.AddMeshInstance(registry, sphereMesh1, Point3(-120.0f, 380.0f, 60.0f),
                          Material::Lambertian(Color(0.75f, 0.2f, 0.85f)).Textured(brick));

    // --- The overhead light.
    scene.AddQuad(registry, Point3(123.0f, 554.0f, 147.0f),
                  Vec3(300.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 265.0f),
                  Material::Emissive(Color(1.0f, 1.0f, 1.0f), 5.0f));

    const MeshHandle sphereMesh2 = scene.meshes.Add(
        Mesh::CreateSphere(50.0f, 32, 16, Material::Lambertian(Color(0.7f, 0.3f, 0.1f))));

    // --- Hero spheres.
    // The book gives this one motion blur; static here (see note above).
    scene.AddMeshInstance(registry, sphereMesh2, Point3(400.0f, 400.0f, 200.0f),
                          Material::Lambertian(Color(0.7f, 0.3f, 0.1f)));

    const MeshHandle sphereMesh3 = scene.meshes.Add(
        Mesh::CreateSphere(50.0f, 32, 16, Material::Dielectric(1.5f)));
    scene.AddMeshInstance(registry, sphereMesh3, Point3(260.0f, 150.0f, 45.0f),
                          Material::Dielectric(1.5f));

    const MeshHandle sphereMesh4 = scene.meshes.Add(
        Mesh::CreateSphere(50.0f, 32, 16, Material::Lambertian(Color(0.7f, 0.3f, 0.1f))));
    scene.AddMeshInstance(registry, sphereMesh4, Point3(0.0f, 150.0f, 145.0f),
                          Material::Metal(Color(0.8f, 0.8f, 0.9f), 0.0f));

    // --- Subsurface scattering: a glass shell with a dense blue medium inside.
    // Both spheres share a centre and radius - the dielectric is the visible
    // surface, the volume is what fills it.
    scene.AddSphere(registry, Point3(360.0f, 150.0f, 145.0f), 70.0f, Material::Dielectric(1.5f));
    // scene.AddVolume(registry, Point3(360.0f, 150.0f, 145.0f), 70.0f, Color(0.2f, 0.4f, 0.9f), 0.2f);

    // --- Global haze. Very low density over a very large radius: individually
    // each ray rarely scatters, but across the whole scene it lifts the blacks
    // and softens everything in the distance.
    // scene.AddVolume(registry, Point3(0.0f, 0.0f, 0.0f), 5000.0f, Color(1.0f, 1.0f, 1.0f), 0.0001f);

    // --- Textured sphere (earth map in the book).
    scene.AddSphere(registry, Point3(700.0f, 500.0f, 400.0f), 100.0f,
                    Material::Lambertian(Color(1.0f, 1.0f, 1.0f)));

    // --- Perlin-noise sphere in the book; plain matte here.
    scene.AddSphere(registry, Point3(220.0f, 280.0f, 300.0f), 80.0f,
                    Material::Lambertian(Color(0.7f, 0.7f, 0.75f)));

    // --- A cluster of 1000 small spheres, built at the origin then rotated and
    // translated into place as one rigid body.
    const Material clusterWhite = Material::Lambertian(Color(0.73f, 0.73f, 0.73f));
    const size_t clusterFirst = scene.Count();
    for (int j = 0; j < 1000; j++)
    {
        scene.AddSphere(
            registry,
            Point3(165.0f * SDL_randf(), 165.0f * SDL_randf(), 165.0f * SDL_randf()),
            10.0f, clusterWhite);
    }
    scene.GroupSince(registry, clusterFirst)
        .RotateY(static_cast<float>(degrees_to_radians(15.0)))
        .Translate(Vec3(-100.0f, 270.0f, 395.0f))
        .MarkRestPose();
}
