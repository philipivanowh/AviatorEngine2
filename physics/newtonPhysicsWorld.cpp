#include "physics/newtonPhysicsWorld.h"

// Python.h (through pybind11) comes first on purpose: it insists on being the
// first include, and embed.h is what gives us initialize_interpreter(PyConfig*)
// so the interpreter can be pointed at a venv the engine did not launch from.
#include <pybind11/embed.h>
#include <pybind11/numpy.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <entt/entt.hpp>

#include "math/quat.hpp"
#include "math/vec3.h"
#include "scene/components.h"

// The Newton backend: components in, bridge calls out, TransformComponent back.
//
// Everything Python is confined to this file (docs/newton_physics.md, rule 3).
// The three things worth knowing before reading it:
//
//   1. One interpreter per process, started on first use and kept alive until
//      the process exits. See PythonRuntime below for why it is not finalized.
//   2. Nothing is marshalled per body per frame. Poses, wrenches, impulses and
//      kinematic targets live in std::vector<float> buffers that numpy wraps in
//      place, so a step is a handful of Python calls no matter how many bodies
//      there are - see NumpyView().
//   3. A Python failure never propagates. It is logged once and the world goes
//      inert: Update stops calling into the bridge and reports no motion, and
//      the app keeps running with a frozen scene rather than dying at frame 900.
//
// The bridge contract this file calls is in docs/newton_physics.md; the Python
// side that implements it is physics/newton/aviator_newton.py.

namespace py = pybind11;
namespace fs = std::filesystem;

namespace
{

    // ---------------------------------------------------------------- logging

    // Both log helpers exist so the "Physics:" prefix and the Python traceback
    // formatting are in one place; every message the backend emits goes through
    // SDL_Log, which is where the rest of the engine's diagnostics go.
    void LogPythonError(const char *what, py::error_already_set &error)
    {
        // error.what() is the exception type, its message and the traceback,
        // which is the only useful thing when the failure is 30 frames deep in
        // Warp. It needs the GIL, so every caller catches inside a gil scope.
        SDL_Log("Physics: %s raised a Python exception:\n%s", what, error.what());
    }

    // ------------------------------------------------------- the interpreter

    std::string TrimSpace(const std::string &text)
    {
        const size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
            return std::string();
        }
        const size_t last = text.find_last_not_of(" \t\r\n");
        return text.substr(first, last - first + 1);
    }

    // One "key = value" line out of an INI-ish file, empty when absent. Used for
    // pyvenv.cfg's "home", which is the only way to find the base installation's
    // standard library from inside a venv - a venv has no Lib/ of its own.
    std::string ConfigValue(const fs::path &file, const std::string &key)
    {
        std::ifstream stream(file);
        if (!stream)
        {
            return std::string();
        }
        std::string line;
        while (std::getline(stream, line))
        {
            const size_t equals = line.find('=');
            if (equals == std::string::npos)
            {
                continue;
            }
            if (TrimSpace(line.substr(0, equals)) == key)
            {
                return TrimSpace(line.substr(equals + 1));
            }
        }
        return std::string();
    }

    // "python311", the stdlib layout suffix. Taken from the headers we compiled
    // against, which is by construction the version of the DLL we link, so the
    // paths below can never name a stdlib from a different Python.
    std::string PythonTag()
    {
        return "python" + std::to_string(PY_MAJOR_VERSION) + std::to_string(PY_MINOR_VERSION);
    }

    std::string PythonDottedTag()
    {
        return "python" + std::to_string(PY_MAJOR_VERSION) + "." + std::to_string(PY_MINOR_VERSION);
    }

    // Where the interpreter lives, and the one interpreter itself.
    //
    // Start() is idempotent and process-wide: CPython cannot be initialized
    // twice in one process (sub-interpreters are not an option - numpy and Warp
    // both hold per-process state), so the first world to start decides which
    // environment the process uses and later worlds reuse it.
    //
    // The interpreter is deliberately NOT finalized at exit. Warp keeps CUDA
    // contexts, a recorded CUDA graph and its own module cache alive in Python
    // objects, and tearing CPython down under them runs Warp's __del__ hooks
    // against a half-destroyed interpreter while the CUDA driver is shutting
    // down: the observed result is a hang or an access violation during exit,
    // long after the last frame. Leaking the interpreter costs nothing (the
    // process is ending) and is what every embedder that loads CUDA through
    // Python ends up doing. AVIATOR_NEWTON_FINALIZE=1 opts back in, which is
    // how the claim above stays testable.
    struct PythonRuntime
    {
        bool running = false;
        bool finalizeAtExit = false;
        std::string environment; // the venv (or plain install) we started with

        // Held so that no thread owns the GIL between calls; every entry point
        // takes it with py::gil_scoped_acquire. Never deleted unless we
        // finalize, because deleting it re-acquires the GIL.
        py::gil_scoped_release *released = nullptr;
    };

    PythonRuntime &Runtime()
    {
        static PythonRuntime runtime;
        return runtime;
    }

    // Resolution order, exactly as documented on PhysicsSettings: the setting,
    // then the environment variable, then what CMake found at configure time.
    std::string ResolveEnvironmentDir(const PhysicsSettings &settings)
    {
        if (!settings.pythonEnvironment.empty())
        {
            return settings.pythonEnvironment;
        }
        if (const char *fromEnv = SDL_getenv("AVIATOR_NEWTON_VENV"); fromEnv != nullptr && *fromEnv != '\0')
        {
            return fromEnv;
        }
#ifdef AVIATOR_NEWTON_VENV
        return AVIATOR_NEWTON_VENV;
#else
        return std::string();
#endif
    }

    // The directory holding aviator_newton.py. Baked in by CMake; the override
    // exists so a bridge can be edited and tried without reconfiguring.
    std::string ResolveBridgeDir()
    {
        if (const char *fromEnv = SDL_getenv("AVIATOR_NEWTON_BRIDGE_DIR"); fromEnv != nullptr && *fromEnv != '\0')
        {
            return fromEnv;
        }
#ifdef AVIATOR_NEWTON_BRIDGE_DIR
        return AVIATOR_NEWTON_BRIDGE_DIR;
#else
        return std::string();
#endif
    }

    // Starts the interpreter, or explains in `error` why it cannot.
    //
    // sys.path is built by hand rather than left to CPython's own search. An
    // interpreter embedded in build-newton/bin/app.exe has no standard library
    // anywhere near it, so the default search finds nothing (or worse, finds an
    // unrelated Python on the machine); site_import stays off so the base
    // installation's site-packages can never shadow the venv's newton/warp.
    bool StartInterpreter(const std::string &environmentDir, std::string &error)
    {
        PythonRuntime &runtime = Runtime();
        if (runtime.running)
        {
            if (!environmentDir.empty() && environmentDir != runtime.environment)
            {
                SDL_Log("Physics: the embedded interpreter is already running from '%s'; "
                        "ignoring the request for '%s' (one interpreter per process)",
                        runtime.environment.c_str(), environmentDir.c_str());
            }
            return true;
        }

        if (environmentDir.empty())
        {
            error = "no Python environment is configured. Build with AVIATOR_WITH_NEWTON=ON, "
                    "set AVIATOR_NEWTON_VENV, or fill PhysicsSettings::pythonEnvironment";
            return false;
        }

        const fs::path environment = fs::path(environmentDir);
        if (!fs::is_directory(environment))
        {
            error = "the Python environment '" + environmentDir + "' does not exist. Create it with:\n"
                    "    python -m venv .venv-newton\n"
                    "    .venv-newton\\Scripts\\python -m pip install newton-physics";
            return false;
        }

#ifdef _WIN32
        const fs::path interpreter = environment / "Scripts" / "python.exe";
        const fs::path sitePackages = environment / "Lib" / "site-packages";
#else
        const fs::path interpreter = environment / "bin" / "python3";
        const fs::path sitePackages = environment / "lib" / PythonDottedTag() / "site-packages";
#endif
        if (!fs::exists(interpreter))
        {
            error = "'" + environmentDir + "' is not a Python environment: " + interpreter.string() +
                    " is missing. Create it with:\n"
                    "    python -m venv .venv-newton\n"
                    "    .venv-newton\\Scripts\\python -m pip install newton-physics";
            return false;
        }

        // A venv has no standard library of its own: pyvenv.cfg's "home" points
        // at the installation that does. Without pyvenv.cfg we were handed a
        // plain installation, which is its own home.
        fs::path home = environment;
        const std::string configured = ConfigValue(environment / "pyvenv.cfg", "home");
        if (!configured.empty())
        {
            home = fs::path(configured);
        }

#ifdef _WIN32
        const fs::path stdlib = home / "Lib";
        const fs::path dynload = home / "DLLs";
#else
        const fs::path stdlib = home / "lib" / PythonDottedTag();
        const fs::path dynload = stdlib / "lib-dynload";
#endif
        if (!fs::exists(stdlib / "os.py"))
        {
            error = "the Python standard library is not where '" + (environment / "pyvenv.cfg").string() +
                    "' says it is: " + (stdlib / "os.py").string() + " is missing (home = '" + home.string() +
                    "'). The base Python installation the environment was created from has moved or been removed";
            return false;
        }
        if (!fs::is_directory(sitePackages))
        {
            error = "'" + environmentDir + "' has no site-packages at " + sitePackages.string();
            return false;
        }

        const std::string bridgeDir = ResolveBridgeDir();
        if (bridgeDir.empty() || !fs::exists(fs::path(bridgeDir) / "aviator_newton.py"))
        {
            error = "the Python bridge aviator_newton.py was not found in '" +
                    (bridgeDir.empty() ? std::string("<unset>") : bridgeDir) +
                    "'. Reconfigure CMake, or set AVIATOR_NEWTON_BRIDGE_DIR to the directory holding it";
            return false;
        }

        PyConfig config;
        // Isolated: no PYTHONPATH, PYTHONHOME or user site directory from the
        // ambient environment can redirect an engine build at someone's shell.
        PyConfig_InitIsolatedConfig(&config);
        config.module_search_paths_set = 1;
        config.site_import = 0;
        config.install_signal_handlers = 0;

        bool ok = true;
        const auto setString = [&](wchar_t **target, const fs::path &value)
        {
            if (!ok)
            {
                return;
            }
            const std::wstring wide = value.wstring();
            ok = !PyStatus_Exception(PyConfig_SetString(&config, target, wide.c_str()));
        };
        const auto appendPath = [&](const fs::path &value)
        {
            if (!ok)
            {
                return;
            }
            const std::wstring wide = value.wstring();
            ok = !PyStatus_Exception(PyWideStringList_Append(&config.module_search_paths, wide.c_str()));
        };

        // program_name/executable make sys.executable useful (Warp shells out to
        // nothing, but numpy and warnings both report it) without letting
        // CPython's path calculation run.
        setString(&config.program_name, interpreter);
        setString(&config.executable, interpreter);
        setString(&config.home, home);
        setString(&config.prefix, environment);
        setString(&config.exec_prefix, environment);
        appendPath(home / (PythonTag() + ".zip")); // may not exist; harmless
        appendPath(stdlib);
        appendPath(dynload);
        appendPath(sitePackages);
        appendPath(fs::path(bridgeDir));
        if (!ok)
        {
            PyConfig_Clear(&config);
            error = "PyConfig rejected the interpreter paths for '" + environmentDir + "'";
            return false;
        }

        try
        {
            // Takes ownership of nothing: it clears the config itself and throws
            // std::runtime_error instead of calling exit() when init fails,
            // which is the whole reason for going through pybind11 here.
            py::initialize_interpreter(&config, 0, nullptr, false);
        }
        catch (const std::exception &failure)
        {
            error = std::string("the embedded interpreter failed to start: ") + failure.what();
            return false;
        }

        runtime.running = true;
        runtime.environment = environmentDir;
        if (const char *flag = SDL_getenv("AVIATOR_NEWTON_FINALIZE"); flag != nullptr && *flag == '1')
        {
            runtime.finalizeAtExit = true;
        }
        // Drop the GIL the interpreter handed us; from here on every call into
        // Python takes it explicitly.
        runtime.released = new py::gil_scoped_release();

        SDL_Log("Physics: embedded Python %d.%d from '%s' (stdlib '%s')",
                PY_MAJOR_VERSION, PY_MINOR_VERSION, environmentDir.c_str(), stdlib.string().c_str());
        return true;
    }

    // Only ever reached when AVIATOR_NEWTON_FINALIZE=1 - see PythonRuntime.
    void FinalizeInterpreterIfAsked()
    {
        PythonRuntime &runtime = Runtime();
        if (!runtime.running)
        {
            return;
        }

        // Take the GIL back whether or not we finalize. pybind11 keeps its
        // internals in Python dicts owned by statics, and those are released at
        // process exit - long after the last world. dec_ref asserts the GIL is
        // held, and by then nothing else would be holding it.
        delete runtime.released;
        runtime.released = nullptr;

        if (!runtime.finalizeAtExit)
        {
            return; // leaking the interpreter is the default - see PythonRuntime
        }

        runtime.running = false;
        SDL_Log("Physics: finalizing the embedded interpreter (AVIATOR_NEWTON_FINALIZE=1)");
        py::finalize_interpreter();
        SDL_Log("Physics: interpreter finalized");
    }

    // ------------------------------------------------------ buffers and poses

    // A numpy array that borrows C++ memory instead of copying it. Passing any
    // object as the base is what stops numpy from taking a copy (pybind11 copies
    // when the base is null) and marks the result writeable, which the bridge
    // requires of its output arrays. The view is only valid for the duration of
    // the call it is passed to, which is exactly what the contract allows.
    template <typename T>
    py::array_t<T> NumpyView(std::vector<T> &data, size_t rowCount, size_t columnCount)
    {
        const py::ssize_t item = static_cast<py::ssize_t>(sizeof(T));
        const py::ssize_t rows = static_cast<py::ssize_t>(rowCount);
        const py::ssize_t columns = static_cast<py::ssize_t>(columnCount);
        if (columns == 1)
        {
            return py::array_t<T>({rows}, {item}, data.data(), py::none());
        }
        return py::array_t<T>({rows, columns}, {columns * item, item}, data.data(), py::none());
    }

    // Poses in the buffers are the bridge's layout: px, py, pz, qx, qy, qz, qw.
    constexpr size_t kPoseStride = 7;
    constexpr size_t kWrenchStride = 6;

    Point3 PositionAt(const std::vector<float> &poses, size_t index)
    {
        const float *pose = poses.data() + index * kPoseStride;
        return Point3(pose[0], pose[1], pose[2]);
    }

    Quat<float> RotationAt(const std::vector<float> &poses, size_t index)
    {
        const float *pose = poses.data() + index * kPoseStride;
        // The five-argument constructor is the raw x, y, z, w one; the
        // four-argument one takes (w, x, y, z). See math/quat.hpp.
        return Quat<float>(pose[3], pose[4], pose[5], pose[6], 0);
    }

    void StorePose(std::vector<float> &poses, size_t index, const Point3 &position, const Quat<float> &rotation)
    {
        float *pose = poses.data() + index * kPoseStride;
        pose[0] = position.x;
        pose[1] = position.y;
        pose[2] = position.z;
        pose[3] = rotation.x;
        pose[4] = rotation.y;
        pose[5] = rotation.z;
        pose[6] = rotation.w;
    }

    const char *SolverName(PhysicsSolver solver)
    {
        switch (solver)
        {
        case PhysicsSolver::XPBD:
            return "xpbd";
        case PhysicsSolver::MuJoCo:
            return "mujoco";
        case PhysicsSolver::Featherstone:
            return "featherstone";
        case PhysicsSolver::SemiImplicit:
            return "semi_implicit";
        }
        return "xpbd";
    }

    py::dict BuildSettingsDict(const PhysicsSettings &settings)
    {
        py::dict dict;
        dict["gravity"] = py::make_tuple(settings.gravity.x, settings.gravity.y, settings.gravity.z);
        dict["fixed_dt"] = settings.fixedTimeStep;
        dict["substeps"] = settings.substeps;
        dict["solver"] = SolverName(settings.solver);
        dict["solver_iterations"] = settings.solverIterations;
        dict["device"] = settings.useGpu ? "cuda" : "cpu";
        dict["use_cuda_graph"] = settings.useCudaGraph;
        return dict;
    }

    // ------------------------------------------------------------- the world

    class NewtonPhysicsWorld final : public IPhysicsWorld
    {
    public:
        // Creating the bridge World validates the settings and gives us a
        // description before anything is built; it allocates nothing on the GPU
        // (that is finalize()'s job), so this stays cheap. Throws on a Python
        // error, which CreateNewtonPhysicsWorld turns into a null return.
        NewtonPhysicsWorld(const PhysicsSettings &settings, py::object bridgeModule)
            : settings(settings), module(std::move(bridgeModule))
        {
            settingsDict = BuildSettingsDict(settings);
            world = module.attr("World")(settingsDict);
            description = py::cast<std::string>(world.attr("description")());
            ++liveWorlds;
        }

        ~NewtonPhysicsWorld() override
        {
            {
                // Every Python object this world owns has to be released while
                // the interpreter is still up, so the scope ends before the
                // optional finalize below.
                py::gil_scoped_acquire gil;
                try
                {
                    if (world)
                    {
                        world.attr("close")();
                    }
                }
                catch (py::error_already_set &failure)
                {
                    LogPythonError("World.close()", failure);
                }
                catch (const std::exception &failure)
                {
                    SDL_Log("Physics: World.close() failed: %s", failure.what());
                }
                world = py::object();
                settingsDict = py::dict();
                module = py::object();
            }
            if (--liveWorlds == 0)
            {
                FinalizeInterpreterIfAsked();
            }
        }

        bool Build(entt::registry &registry) override
        {
            py::gil_scoped_acquire gil;

            // A rebuild is also how a world recovers from a failure, so the
            // inert flag is cleared here rather than being permanent. Pausing is
            // not touched: it is the caller's state, not the scene's.
            inert = false;

            const Uint64 started = SDL_GetPerformanceCounter();
            try
            {
                ReleaseWorld();
                ClearScene();
                world = module.attr("World")(settingsDict);

                CollectParts(registry);
                AddBodies(registry);
                AddStaticGeometry(registry);

                if (bodies.empty())
                {
                    SDL_Log("Physics: no rigid bodies in the registry - Newton will simulate static geometry only");
                }

                SDL_Log("Physics: finalizing Newton with %zu bodies, %d colliders "
                        "(compiles GPU kernels, can take a minute the first time)",
                        bodies.size(), colliderCount);
                const Uint64 beforeFinalize = SDL_GetPerformanceCounter();
                world.attr("finalize")();
                const float finalizeMs = MillisecondsSince(beforeFinalize);

                description = py::cast<std::string>(world.attr("description")());
                ReadPoses();
                previousPose = pose;
                restPose = pose;

                SDL_Log("Physics: finalize took %.0f ms, total build %.0f ms - %s",
                        finalizeMs, MillisecondsSince(started), description.c_str());
            }
            catch (py::error_already_set &failure)
            {
                LogPythonError("Build", failure);
                GoInert("Build");
                return false;
            }
            catch (const std::exception &failure)
            {
                SDL_Log("Physics: Build failed: %s", failure.what());
                GoInert("Build");
                return false;
            }

            accumulator = 0.0f;
            reportMoved = false;
            return true;
        }

        PhysicsStepResult Update(entt::registry &registry, float frameSeconds) override
        {
            PhysicsStepResult result;
            if (paused || inert || bodies.empty())
            {
                return result;
            }

            const Uint64 started = SDL_GetPerformanceCounter();

            // Fixed steps, with the excess dropped rather than queued: a frame
            // that took 200 ms must not hand the next frame four steps of debt.
            const float fixedStep = std::max(settings.fixedTimeStep, 1.0e-6f);
            accumulator += std::max(frameSeconds, 0.0f);
            const int wanted = static_cast<int>(accumulator / fixedStep);
            const int steps = std::min(wanted, static_cast<int>(settings.maxStepsPerUpdate));
            accumulator = std::max(accumulator - static_cast<float>(wanted) * fixedStep, 0.0f);

            {
                py::gil_scoped_acquire gil;
                try
                {
                    // Impulses and velocity writes change state now, so they are
                    // flushed even on a frame that runs no step. Wrenches are
                    // not: they act during a step, and a step is what consumes
                    // them.
                    FlushImpulses();
                    FlushVelocities();
                    PushKinematicTargets(registry);

                    if (steps > 0)
                    {
                        if (wrenchPending)
                        {
                            world.attr("set_wrenches")(NumpyView(wrench, bodies.size(), kWrenchStride));
                        }
                        if (settings.interpolate)
                        {
                            world.attr("step")(steps, NumpyView(previousPose, bodies.size(), kPoseStride));
                        }
                        else
                        {
                            world.attr("step")(steps);
                        }
                        ReadPoses();
                    }
                    // steps == 0 keeps the poses it has: the frame still writes
                    // transforms, just at a later point between the same two
                    // fixed steps.
                }
                catch (py::error_already_set &failure)
                {
                    LogPythonError("Update", failure);
                    GoInert("Update");
                    return result;
                }
                catch (const std::exception &failure)
                {
                    SDL_Log("Physics: Update failed: %s", failure.what());
                    GoInert("Update");
                    return result;
                }
            }

            // Forces are consumed by the step that applies them, so a frame
            // too fast to run one keeps them queued for the next frame that
            // does. Dropping them here instead would make the net force depend
            // on the frame rate: at 100 fps against a 60 Hz step, two frames in
            // five would push with nothing at all.
            if (steps > 0)
            {
                ClearWrench();
            }

            const float blend = settings.interpolate ? std::min(accumulator / fixedStep, 1.0f) : 1.0f;
            result.moved = WriteTransforms(registry, blend);
            if (reportMoved)
            {
                result.moved = true;
                reportMoved = false;
            }
            result.steps = static_cast<Uint32>(steps);
            result.milliseconds = MillisecondsSince(started);
            return result;
        }

        void Reset(entt::registry &registry) override
        {
            if (inert || bodies.empty())
            {
                return;
            }

            // Rest poses come from the registry, not from a Build-time copy: a
            // scene is allowed to move a RestTransformComponent (an editor drag,
            // a new checkpoint) and Reset has to honour that.
            for (size_t index = 0; index < bodies.size(); ++index)
            {
                const entt::entity entity = bodies[index].entity;
                if (!registry.valid(entity))
                {
                    continue;
                }
                if (const RestTransformComponent *rest = registry.try_get<RestTransformComponent>(entity))
                {
                    StorePose(restPose, index, rest->position, normalize(rest->rotation));
                }
            }

            py::gil_scoped_acquire gil;
            try
            {
                // write_body_poses zeroes every velocity and pending wrench and
                // cancels kinematic targets, so the start velocities go on top.
                world.attr("write_body_poses")(NumpyView(restPose, bodies.size(), kPoseStride));

                velocityIndex.clear();
                velocityLinear.clear();
                velocityAngular.clear();
                for (size_t index = 0; index < bodies.size(); ++index)
                {
                    if (bodies[index].kinematic || !registry.valid(bodies[index].entity))
                    {
                        continue;
                    }
                    const RigidbodyComponent *body = registry.try_get<RigidbodyComponent>(bodies[index].entity);
                    if (body == nullptr)
                    {
                        continue;
                    }
                    velocityIndex.push_back(static_cast<int>(index));
                    velocityLinear.insert(velocityLinear.end(),
                                          {body->linearVelocity.x, body->linearVelocity.y, body->linearVelocity.z});
                    velocityAngular.insert(velocityAngular.end(),
                                           {body->angularVelocity.x, body->angularVelocity.y, body->angularVelocity.z});
                }
                FlushVelocities();
            }
            catch (py::error_already_set &failure)
            {
                LogPythonError("Reset", failure);
                GoInert("Reset");
                return;
            }
            catch (const std::exception &failure)
            {
                SDL_Log("Physics: Reset failed: %s", failure.what());
                GoInert("Reset");
                return;
            }

            // Queued inputs belong to the run that just ended.
            ClearWrench();
            impulseIndex.clear();
            impulseValue.clear();

            pose = restPose;
            previousPose = restPose;
            accumulator = 0.0f;

            // The rest pose is where the kinematic bodies now are, so nothing is
            // pushed as a target until the scene actually moves one.
            for (size_t slot = 0; slot < kinematicBodies.size(); ++slot)
            {
                const size_t index = static_cast<size_t>(kinematicBodies[slot]);
                std::copy_n(restPose.data() + index * kPoseStride, kPoseStride,
                            kinematicSent.data() + slot * kPoseStride);
            }

            WriteRestTransforms(registry);
            reportMoved = true;
        }

        void AddForce(entt::entity body, const Vec3<float> &force) override
        {
            const int index = DynamicIndexOf(body);
            if (index < 0)
            {
                return;
            }
            float *entry = wrench.data() + static_cast<size_t>(index) * kWrenchStride;
            entry[0] += force.x;
            entry[1] += force.y;
            entry[2] += force.z;
            wrenchPending = true;
        }

        void AddTorque(entt::entity body, const Vec3<float> &torque) override
        {
            const int index = DynamicIndexOf(body);
            if (index < 0)
            {
                return;
            }
            float *entry = wrench.data() + static_cast<size_t>(index) * kWrenchStride;
            entry[3] += torque.x;
            entry[4] += torque.y;
            entry[5] += torque.z;
            wrenchPending = true;
        }

        void AddImpulse(entt::entity body, const Vec3<float> &impulse) override
        {
            const int index = DynamicIndexOf(body);
            if (index < 0)
            {
                return;
            }
            // Repeated indices add up on the Python side, so no search here.
            impulseIndex.push_back(index);
            impulseValue.insert(impulseValue.end(), {impulse.x, impulse.y, impulse.z});
        }

        void SetVelocity(entt::entity body, const Vec3<float> &linear, const Vec3<float> &angular) override
        {
            const int index = DynamicIndexOf(body);
            if (index < 0)
            {
                return;
            }
            velocityIndex.push_back(index);
            velocityLinear.insert(velocityLinear.end(), {linear.x, linear.y, linear.z});
            velocityAngular.insert(velocityAngular.end(), {angular.x, angular.y, angular.z});
        }

        void SetPaused(bool value) override { paused = value; }
        bool Paused() const override { return paused; }

        size_t BodyCount() const override { return bodies.size(); }
        const char *Description() const override { return description.c_str(); }

    private:
        // A built body, in bridge index order: body i is the i-th add_body call.
        struct Body
        {
            entt::entity entity = entt::null;
            bool kinematic = false;
        };

        // A collider entity that is one part of a body. Its transform is the
        // body's pose composed with this local pose, every frame.
        struct Part
        {
            entt::entity entity = entt::null;
            size_t body = 0; // index into bodies
            Point3 localPosition;
            Quat<float> localRotation;
        };

        // -------------------------------------------------------- build steps

        void ClearScene()
        {
            bodies.clear();
            parts.clear();
            bodyIndexOf.clear();
            partsOfBody.clear();
            kinematicBodies.clear();
            colliderCount = 0;

            pose.clear();
            previousPose.clear();
            restPose.clear();
            wrench.clear();
            kinematicSent.clear();
            ClearWrench();
            impulseIndex.clear();
            impulseValue.clear();
            velocityIndex.clear();
            velocityLinear.clear();
            velocityAngular.clear();
        }

        void ReleaseWorld()
        {
            if (!world)
            {
                return;
            }
            try
            {
                world.attr("close")();
            }
            catch (py::error_already_set &failure)
            {
                // Not fatal: the old world is being dropped either way.
                LogPythonError("World.close() during rebuild", failure);
            }
            world = py::object();
        }

        // Which collider entities belong to which body. Built first so a body
        // can be skipped before it is ever added to the bridge.
        void CollectParts(entt::registry &registry)
        {
            for (auto [entity, collider, shapeOf] : registry.view<ColliderComponent, ShapeOfComponent>().each())
            {
                if (registry.all_of<RigidbodyComponent>(entity))
                {
                    // Both a body and somebody else's part. Being a body wins -
                    // the alternative is one collider owned twice, with one
                    // collider_ID for two shapes.
                    SDL_Log("Physics: entity %u has a RigidbodyComponent and a ShapeOfComponent; "
                            "treating it as a body of its own and ignoring the ShapeOf",
                            static_cast<unsigned>(entt::to_integral(entity)));
                    continue;
                }
                if (shapeOf.body == entt::null || !registry.valid(shapeOf.body))
                {
                    SDL_Log("Physics: skipping collider entity %u - its ShapeOfComponent names a body that "
                            "does not exist",
                            static_cast<unsigned>(entt::to_integral(entity)));
                    continue;
                }
                if (!registry.all_of<RigidbodyComponent>(shapeOf.body))
                {
                    SDL_Log("Physics: skipping collider entity %u - entity %u has no RigidbodyComponent, "
                            "so it is not a body",
                            static_cast<unsigned>(entt::to_integral(entity)),
                            static_cast<unsigned>(entt::to_integral(shapeOf.body)));
                    continue;
                }
                partsOfBody[shapeOf.body].push_back(entity);
            }
        }

        void AddBodies(entt::registry &registry)
        {
            const py::object addBody = world.attr("add_body");

            // Gathered before anything is added, because the loop below writes
            // RestTransformComponent - creating a component type mid-iteration
            // is the one registry edit a view does not promise to survive.
            std::vector<entt::entity> candidates;
            for (auto entity : registry.view<RigidbodyComponent, TransformComponent>())
            {
                candidates.push_back(entity);
            }

            for (const entt::entity entity : candidates)
            {
                RigidbodyComponent &body = registry.get<RigidbodyComponent>(entity);
                const TransformComponent &transform = registry.get<TransformComponent>(entity);

                ColliderComponent *own = registry.try_get<ColliderComponent>(entity);
                const bool ownIsPart = own != nullptr && registry.all_of<ShapeOfComponent>(entity);
                const auto found = partsOfBody.find(entity);
                const size_t partCount = found == partsOfBody.end() ? 0 : found->second.size();
                if (own == nullptr && partCount == 0)
                {
                    SDL_Log("Physics: skipping body entity %u - it has no ColliderComponent and no "
                            "ShapeOfComponent parts, so there is nothing to collide with",
                            static_cast<unsigned>(entt::to_integral(entity)));
                    continue;
                }

                const bool kinematic = body.type == RigidbodyType::Kinematic;
                const Quat<float> rotation = normalize(transform.rotation);
                const int index = py::cast<int>(addBody(
                    kinematic ? 1 : 0,
                    py::make_tuple(transform.position.x, transform.position.y, transform.position.z),
                    py::make_tuple(rotation.x, rotation.y, rotation.z, rotation.w),
                    body.mass,
                    py::make_tuple(body.linearVelocity.x, body.linearVelocity.y, body.linearVelocity.z),
                    py::make_tuple(body.angularVelocity.x, body.angularVelocity.y, body.angularVelocity.z)));

                body.physicsBody_ID = index;
                bodies.push_back(Body{entity, kinematic});
                bodyIndexOf[entity] = index;
                if (kinematic)
                {
                    kinematicBodies.push_back(index);
                }
                registry.emplace_or_replace<RestTransformComponent>(
                    entity, RestTransformComponent{transform.position, rotation});

                if (own != nullptr && !ownIsPart)
                {
                    // The body's own collider sits at the body origin.
                    AddCollider(index, *own, Point3(0.0f, 0.0f, 0.0f), Quat<float>());
                }
                for (size_t part = 0; part < partCount; ++part)
                {
                    const entt::entity partEntity = found->second[part];
                    ColliderComponent &collider = registry.get<ColliderComponent>(partEntity);
                    const ShapeOfComponent &shapeOf = registry.get<ShapeOfComponent>(partEntity);
                    const Quat<float> localRotation = normalize(shapeOf.localRotation);
                    AddCollider(index, collider, shapeOf.localPosition, localRotation);

                    parts.push_back(Part{partEntity, bodies.size() - 1, shapeOf.localPosition, localRotation});
                    // A part's rest pose is the part's own transform, so a Reset
                    // that happens before the first Update still has something
                    // sensible on the entity.
                    if (const TransformComponent *partTransform = registry.try_get<TransformComponent>(partEntity))
                    {
                        registry.emplace_or_replace<RestTransformComponent>(
                            partEntity,
                            RestTransformComponent{partTransform->position, normalize(partTransform->rotation)});
                    }
                }
            }

            const size_t count = bodies.size();
            pose.assign(count * kPoseStride, 0.0f);
            previousPose.assign(count * kPoseStride, 0.0f);
            restPose.assign(count * kPoseStride, 0.0f);
            wrench.assign(count * kWrenchStride, 0.0f);
            kinematicSent.assign(kinematicBodies.size() * kPoseStride, 0.0f);
            wrenchPending = false;
        }

        // Everything that collides but is not part of a body: a ColliderComponent
        // with a TransformComponent and no rigidbody. Its transform is a world
        // pose, which is what body = -1 means to the bridge.
        void AddStaticGeometry(entt::registry &registry)
        {
            std::vector<entt::entity> candidates;
            for (auto entity : registry.view<ColliderComponent, TransformComponent>(
                     entt::exclude<RigidbodyComponent, ShapeOfComponent>))
            {
                candidates.push_back(entity);
            }

            for (const entt::entity entity : candidates)
            {
                ColliderComponent &collider = registry.get<ColliderComponent>(entity);
                const TransformComponent &transform = registry.get<TransformComponent>(entity);
                const Quat<float> rotation = normalize(transform.rotation);
                AddCollider(-1, collider, transform.position, rotation);
                registry.emplace_or_replace<RestTransformComponent>(
                    entity, RestTransformComponent{transform.position, rotation});
            }
        }

        void AddCollider(int body, ColliderComponent &collider, const Point3 &position, const Quat<float> &rotation)
        {
            const int id = py::cast<int>(world.attr("add_collider")(
                body,
                static_cast<int>(collider.shape),
                py::make_tuple(position.x, position.y, position.z),
                py::make_tuple(rotation.x, rotation.y, rotation.z, rotation.w),
                py::make_tuple(collider.halfExtent.x, collider.halfExtent.y, collider.halfExtent.z),
                collider.radius,
                collider.halfHeight,
                collider.friction,
                collider.restitution));
            collider.collider_ID = id;
            ++colliderCount;
        }

        // ------------------------------------------------------- update steps

        void ReadPoses()
        {
            world.attr("read_body_poses")(NumpyView(pose, bodies.size(), kPoseStride));
        }

        void FlushImpulses()
        {
            if (impulseIndex.empty())
            {
                return;
            }
            const size_t rows = impulseIndex.size();
            world.attr("apply_impulses")(NumpyView(impulseIndex, rows, 1), NumpyView(impulseValue, rows, 3));
            impulseIndex.clear();
            impulseValue.clear();
        }

        void FlushVelocities()
        {
            if (velocityIndex.empty())
            {
                return;
            }
            const size_t rows = velocityIndex.size();
            world.attr("set_body_velocities")(NumpyView(velocityIndex, rows, 1),
                                              NumpyView(velocityLinear, rows, 3),
                                              NumpyView(velocityAngular, rows, 3));
            velocityIndex.clear();
            velocityLinear.clear();
            velocityAngular.clear();
        }

        // Kinematic bodies are the one component read rather than written. Only
        // the ones whose transform actually changed are sent: a kinematic body
        // with no new target holds still, so re-sending an identical pose every
        // frame would cost an upload for nothing.
        void PushKinematicTargets(entt::registry &registry)
        {
            kinematicIndex.clear();
            kinematicPose.clear();
            for (size_t slot = 0; slot < kinematicBodies.size(); ++slot)
            {
                const int index = kinematicBodies[slot];
                const entt::entity entity = bodies[static_cast<size_t>(index)].entity;
                if (!registry.valid(entity))
                {
                    continue;
                }
                const TransformComponent *transform = registry.try_get<TransformComponent>(entity);
                if (transform == nullptr)
                {
                    continue;
                }
                const Quat<float> rotation = normalize(transform->rotation);
                const float target[kPoseStride] = {transform->position.x, transform->position.y,
                                                   transform->position.z, rotation.x,
                                                   rotation.y,            rotation.z,
                                                   rotation.w};
                float *sent = kinematicSent.data() + slot * kPoseStride;
                if (std::equal(target, target + kPoseStride, sent))
                {
                    continue;
                }
                std::copy_n(target, kPoseStride, sent);
                kinematicIndex.push_back(index);
                kinematicPose.insert(kinematicPose.end(), target, target + kPoseStride);
            }
            if (kinematicIndex.empty())
            {
                return;
            }
            const size_t rows = kinematicIndex.size();
            world.attr("set_kinematic_poses")(NumpyView(kinematicIndex, rows, 1),
                                              NumpyView(kinematicPose, rows, kPoseStride));
        }

        // The interpolated world pose of body `index`, and whether it is one the
        // engine is allowed to write.
        void BodyPose(size_t index, float blend, Point3 &position, Quat<float> &rotation) const
        {
            const Point3 current = PositionAt(pose, index);
            const Quat<float> currentRotation = RotationAt(pose, index);
            if (blend >= 1.0f)
            {
                position = current;
                rotation = currentRotation;
                return;
            }
            const Point3 last = PositionAt(previousPose, index);
            position = last + blend * (current - last);
            rotation = normalize(slerp(RotationAt(previousPose, index), currentRotation, blend));
        }

        bool WriteTransforms(entt::registry &registry, float blend)
        {
            bool moved = false;

            // Bodies first, because the parts below compose against these poses.
            for (size_t index = 0; index < bodies.size(); ++index)
            {
                if (bodies[index].kinematic || !registry.valid(bodies[index].entity))
                {
                    continue;
                }
                TransformComponent *transform = registry.try_get<TransformComponent>(bodies[index].entity);
                if (transform == nullptr)
                {
                    continue;
                }
                Point3 position;
                Quat<float> rotation;
                BodyPose(index, blend, position, rotation);
                moved = WritePose(*transform, position, rotation) || moved;
            }

            for (const Part &part : parts)
            {
                if (!registry.valid(part.entity))
                {
                    continue;
                }
                TransformComponent *transform = registry.try_get<TransformComponent>(part.entity);
                if (transform == nullptr)
                {
                    continue;
                }

                Point3 bodyPosition;
                Quat<float> bodyRotation;
                if (bodies[part.body].kinematic)
                {
                    // A kinematic body's authored transform is the truth, and it
                    // is a whole frame ahead of the pose the solver has reached.
                    // Composing against the solver's pose instead would leave
                    // the parts of a kinematic compound trailing its pivot.
                    const TransformComponent *pivot = registry.try_get<TransformComponent>(bodies[part.body].entity);
                    if (pivot == nullptr)
                    {
                        continue;
                    }
                    bodyPosition = pivot->position;
                    bodyRotation = normalize(pivot->rotation);
                }
                else
                {
                    BodyPose(part.body, blend, bodyPosition, bodyRotation);
                }

                const Point3 position = bodyPosition + rotate(bodyRotation, part.localPosition);
                const Quat<float> rotation = normalize(bodyRotation * part.localRotation);
                moved = WritePose(*transform, position, rotation) || moved;
            }

            return moved;
        }

        // The sleep test, and the only place a TransformComponent is written.
        // Measured against what is on the entity right now rather than against
        // the last pose physics wrote, so a scene that has settled reports no
        // motion even after a rebuild or an external nudge.
        bool WritePose(TransformComponent &transform, const Point3 &position, const Quat<float> &rotation)
        {
            const Vec3<float> delta = position - transform.position;
            if (delta.length() < settings.sleepDistance)
            {
                const float cosHalf = std::min(std::fabs(dot(transform.rotation, rotation)), 1.0f);
                if (2.0f * std::acos(cosHalf) < settings.sleepAngle)
                {
                    return false;
                }
            }
            transform.position = position;
            transform.rotation = rotation;
            return true;
        }

        // Reset writes transforms itself rather than waiting for the next
        // Update, so a reset scene is on screen the same frame.
        void WriteRestTransforms(entt::registry &registry)
        {
            for (size_t index = 0; index < bodies.size(); ++index)
            {
                if (!registry.valid(bodies[index].entity))
                {
                    continue;
                }
                // Kinematic bodies included: this is a teleport back to the
                // authored pose, not a per-frame write, and leaving a kinematic
                // body where the last run left it is not a reset.
                if (TransformComponent *transform = registry.try_get<TransformComponent>(bodies[index].entity))
                {
                    transform->position = PositionAt(restPose, index);
                    transform->rotation = RotationAt(restPose, index);
                }
            }
            for (const Part &part : parts)
            {
                if (!registry.valid(part.entity))
                {
                    continue;
                }
                if (TransformComponent *transform = registry.try_get<TransformComponent>(part.entity))
                {
                    const Point3 bodyPosition = PositionAt(restPose, part.body);
                    const Quat<float> bodyRotation = RotationAt(restPose, part.body);
                    transform->position = bodyPosition + rotate(bodyRotation, part.localPosition);
                    transform->rotation = normalize(bodyRotation * part.localRotation);
                }
            }
        }

        // ------------------------------------------------------------- odds and ends

        int DynamicIndexOf(entt::entity entity) const
        {
            if (inert)
            {
                return -1;
            }
            const auto found = bodyIndexOf.find(entity);
            if (found == bodyIndexOf.end())
            {
                return -1;
            }
            return bodies[static_cast<size_t>(found->second)].kinematic ? -1 : found->second;
        }

        void ClearWrench()
        {
            if (wrenchPending)
            {
                std::fill(wrench.begin(), wrench.end(), 0.0f);
                wrenchPending = false;
            }
        }

        // Logged once, not once per frame: a broken Update would otherwise
        // produce one message every 16 ms for the rest of the session.
        void GoInert(const char *where)
        {
            if (!inert)
            {
                inert = true;
                SDL_Log("Physics: %s failed, so Newton is now inert - the scene will not move. "
                        "Fix the error above and call Build again to recover",
                        where);
            }
        }

        static float MillisecondsSince(Uint64 counter)
        {
            const Uint64 frequency = SDL_GetPerformanceFrequency();
            const Uint64 elapsed = SDL_GetPerformanceCounter() - counter;
            return static_cast<float>(static_cast<double>(elapsed) * 1000.0 / static_cast<double>(frequency));
        }

        PhysicsSettings settings;
        std::string description;

        // Python objects. Released in the destructor, with the GIL held.
        py::object module;
        py::object world;
        py::dict settingsDict;

        std::vector<Body> bodies;
        std::vector<Part> parts;
        std::unordered_map<entt::entity, int> bodyIndexOf;
        std::unordered_map<entt::entity, std::vector<entt::entity>> partsOfBody;
        std::vector<int> kinematicBodies; // bridge indices, in slot order
        int colliderCount = 0;

        // Buffers numpy wraps in place. Sized once per Build; never reallocated
        // during a frame, so the views handed to Python stay cheap.
        std::vector<float> pose;         // (N, 7) current
        std::vector<float> previousPose; // (N, 7) before the last fixed step
        std::vector<float> restPose;     // (N, 7) what Reset returns to
        std::vector<float> wrench;       // (N, 6) accumulated force and torque
        std::vector<float> kinematicSent;
        std::vector<int> kinematicIndex;
        std::vector<float> kinematicPose;
        std::vector<int> impulseIndex;
        std::vector<float> impulseValue;
        std::vector<int> velocityIndex;
        std::vector<float> velocityLinear;
        std::vector<float> velocityAngular;

        float accumulator = 0.0f;
        bool paused = false;
        bool inert = false;
        bool wrenchPending = false;
        bool reportMoved = false; // one forced moved = true, after a Reset

        static int liveWorlds;
    };

    int NewtonPhysicsWorld::liveWorlds = 0;

} // namespace

std::unique_ptr<IPhysicsWorld> CreateNewtonPhysicsWorld(const PhysicsSettings &settings)
{
    const std::string environmentDir = ResolveEnvironmentDir(settings);
    std::string error;
    if (!StartInterpreter(environmentDir, error))
    {
        SDL_Log("Physics: Newton is unavailable - %s", error.c_str());
        return nullptr;
    }

    py::gil_scoped_acquire gil;
    try
    {
        py::object module = py::module_::import("aviator_newton");

        // The version gate is cheap insurance: the bridge and this file are two
        // halves of one contract, and a stale aviator_newton.py on sys.path
        // would otherwise fail much later with a confusing AttributeError.
        const int version = py::cast<int>(module.attr("BRIDGE_VERSION"));
        if (version != 1)
        {
            SDL_Log("Physics: aviator_newton reports BRIDGE_VERSION %d, but this build speaks version 1. "
                    "Update physics/newton/aviator_newton.py or this backend",
                    version);
            return nullptr;
        }

        auto world = std::make_unique<NewtonPhysicsWorld>(settings, std::move(module));
        SDL_Log("Physics: %s", world->Description());
        return world;
    }
    catch (py::error_already_set &failure)
    {
        LogPythonError("starting Newton", failure);
        return nullptr;
    }
    catch (const std::exception &failure)
    {
        SDL_Log("Physics: Newton failed to start: %s", failure.what());
        return nullptr;
    }
}
