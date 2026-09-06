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

    scene.maxDepth = 10;

    const Texture *brick = scene.textures.Load("brick/textures/red_brick_diff_4k.jpg");

    scene.AddSphere(registry, Point3(10.0f, 10.0f, 0.0f), 5.0f, Material::Emissive(Color(0.94f, 0.94f, 0.94f), 5.0f));

    scene.AddSphere(registry,
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
    scene.maxDepth = 10;

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
            scene.AddBox(registry, Point3(x0, 0.0f, z0), Point3(x0 + w, y1, z0 + w), ground);
        }
    }

    // --- The overhead light.
    scene.AddQuad(registry, Point3(123.0f, 554.0f, 147.0f),
                  Vec3(300.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 265.0f),
                  Material::Emissive(Color(1.0f, 1.0f, 1.0f), 7.0f));

    // --- Hero spheres.
    // The book gives this one motion blur; static here (see note above).
    scene.AddSphere(registry, Point3(400.0f, 400.0f, 200.0f), 50.0f,
                    Material::Lambertian(Color(0.7f, 0.3f, 0.1f)));

    scene.AddSphere(registry, Point3(260.0f, 150.0f, 45.0f), 50.0f, Material::Dielectric(1.5f));

    scene.AddSphere(registry, Point3(0.0f, 150.0f, 145.0f), 50.0f,
                    Material::Metal(Color(0.8f, 0.8f, 0.9f), 1.0f));

    // --- Subsurface scattering: a glass shell with a dense blue medium inside.
    // Both spheres share a centre and radius - the dielectric is the visible
    // surface, the volume is what fills it.
    scene.AddSphere(registry, Point3(360.0f, 150.0f, 145.0f), 70.0f, Material::Dielectric(1.5f));
    //scene.AddVolume(registry, Point3(360.0f, 150.0f, 145.0f), 70.0f, Color(0.2f, 0.4f, 0.9f), 0.2f);

    // --- Global haze. Very low density over a very large radius: individually
    // each ray rarely scatters, but across the whole scene it lifts the blacks
    // and softens everything in the distance.
    //scene.AddVolume(registry, Point3(0.0f, 0.0f, 0.0f), 5000.0f, Color(1.0f, 1.0f, 1.0f), 0.0001f);

    // --- Textured sphere (earth map in the book).
    scene.AddSphere(registry, Point3(400.0f, 200.0f, 400.0f), 100.0f,
                    Material::Lambertian(Color(1.0f, 1.0f, 1.0f)).Textured(brick));

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
