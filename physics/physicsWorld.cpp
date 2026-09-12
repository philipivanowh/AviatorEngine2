#include "physics/physicsWorld.h"

#ifdef AVIATOR_WITH_NEWTON
#include "physics/newtonPhysicsWorld.h"
#endif

namespace
{

    // The world for when there is no physics. Everything succeeds and nothing
    // moves, so the rest of the engine never has to check whether physics exists.
    class NullPhysicsWorld final : public IPhysicsWorld
    {
    public:
        explicit NullPhysicsWorld(const char *reason)
            : description(std::string("Null (") + reason + ")")
        {
        }

        bool Build(entt::registry &) override { return true; }
        PhysicsStepResult Update(entt::registry &, float) override { return PhysicsStepResult(); }
        void Reset(entt::registry &) override {}

        void AddForce(entt::entity, const Vec3<float> &) override {}
        void AddTorque(entt::entity, const Vec3<float> &) override {}
        void AddImpulse(entt::entity, const Vec3<float> &) override {}
        void SetVelocity(entt::entity, const Vec3<float> &, const Vec3<float> &) override {}

        void SetPaused(bool value) override { paused = value; }
        bool Paused() const override { return paused; }

        size_t BodyCount() const override { return 0; }
        const char *Description() const override { return description.c_str(); }

    private:
        std::string description;
        bool paused = false;
    };

} // namespace

std::unique_ptr<IPhysicsWorld> CreatePhysicsWorld(const PhysicsSettings &settings)
{
    if (settings.backend == PhysicsBackend::Null)
    {
        return std::make_unique<NullPhysicsWorld>("disabled");
    }

#ifdef AVIATOR_WITH_NEWTON
    // CreateNewtonPhysicsWorld logs its own reason and returns null on failure.
    if (std::unique_ptr<IPhysicsWorld> newton = CreateNewtonPhysicsWorld(settings))
    {
        return newton;
    }
    SDL_Log("Physics: Newton did not start (see above) - running without physics");
    return std::make_unique<NullPhysicsWorld>("Newton failed to start");
#else
    SDL_Log("Physics: built without Newton (AVIATOR_WITH_NEWTON=OFF) - running without physics");
    return std::make_unique<NullPhysicsWorld>("Newton not built");
#endif
}
