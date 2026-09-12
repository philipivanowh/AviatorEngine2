"""aviator_newton - the Python half of AviatorEngine's Newton physics backend.

The C++ backend (physics/newtonPhysicsWorld.cpp) embeds CPython and drives one
:class:`World` from this module. ``World`` owns everything Newton-specific: the
model, the solver, the collision pipeline and the GPU buffers. C++ only ever
hands it numbers and numpy arrays and gets poses back. The contract is written
down in docs/newton_physics.md ("Python bridge contract"); this docstring
explains how it is implemented, for a C++ developer who has not used Newton.

Lifecycle
---------
::

    world = World({"gravity": (0, -9.81, 0), "fixed_dt": 1/60, "substeps": 10,
                   "solver": "xpbd", "solver_iterations": 2,
                   "device": "cuda", "use_cuda_graph": True})
    b = world.add_body(0, (0, 2, 0), (0, 0, 0, 1), 1.0, (0, 0, 0), (0, 0, 0))
    world.add_collider(b, 1, (0, 0, 0), (0, 0, 0, 1), (0.5, 0.5, 0.5), 0.5, 0.5, 0.5, 0.0)
    world.add_collider(-1, 3, (0, 0, 0), (0, 0, 0, 1), (1, 1, 1), 1, 1, 0.5, 0.0)  # ground
    world.finalize()                       # slow the first time: compiles GPU kernels
    poses = numpy.empty((world.body_count(), 7), numpy.float32)
    while running:
        world.set_wrenches(...)            # optional, once per frame
        world.set_kinematic_poses(...)     # optional, once per frame
        world.step(n, previous_poses)      # n fixed steps
        world.read_body_poses(poses)
    world.close()

Conventions
-----------
* SI units (m, kg, s), Y-up. ``newton.ModelBuilder`` is created with
  ``up_axis=Y`` and the gravity vector from the settings.
* Poses are ``float32 (N, 7)``: ``px, py, pz, qx, qy, qz, qw``. That is exactly
  the memory layout of Newton's ``wp.transform`` (``State.body_q``), so poses are
  copied, never converted. Position is the body ORIGIN (the pose passed to
  ``add_body``), not the center of mass.
* Velocities follow Newton's public twist convention: ``(v, w)`` where ``v`` is
  the world-space velocity of the body's CENTER OF MASS and ``w`` the world-space
  angular velocity (see ``State.body_qd`` and ``integrate_rigid_body`` in
  Newton's source). For a body whose colliders are centered on its origin the
  two points coincide. ``body_qd`` stores linear first, angular second.
* Wrenches are ``float32 (N, 6)``: force then torque, world space, about the
  center of mass - the layout of ``State.body_f``.
* Quaternions coming in are normalized; a zero or non-finite quaternion raises.
* Capsules: Newton's capsule runs along its local Z axis and its plane's normal
  is its local +Z axis. The contract wants local Y for both, so the bridge
  appends a fixed -90 degree rotation about X to those shapes' local transform
  (it maps +Z onto +Y). Spheres and boxes need no correction.

Mass by volume
--------------
Newton derives a body's mass, center of mass and inertia tensor from the
density of the shapes attached to it. The contract instead gives one mass per
body, so ``add_body``/``add_collider`` only record their arguments and
``finalize`` materializes them: for each dynamic body it sums the volumes of its
colliders (computed with Newton's own ``compute_inertia_shape``, so the numbers
agree exactly) and gives every collider the density ``mass / total_volume``.
The result is the requested mass, a volume-weighted center of mass and a
correct compound inertia tensor. Overlapping colliders count their shared
volume twice. Kinematic bodies use density 1000 kg/m^3 (their mass never
matters to XPBD). Static colliders (``body = -1``) have no mass.

Stepping, substeps and the CUDA graph
-------------------------------------
One fixed step of ``fixed_dt`` is split into ``substeps`` solver steps. Each
substep does, in this order::

    body_f      <- persistent wrench buffer   (clears forces and adds the external
                                               wrench in one copy)
    kinematic   <- pose/velocity at the end of this substep   (only if any exist)
    collide(state_0) -> contacts
    solver.step(state_0 -> state_1, dt / substeps)
    swap(state_0, state_1)

Newton solvers read ``state_0`` and write ``state_1``; the example code swaps
the two Python references after every substep. When ``use_cuda_graph`` is on
(and the device is CUDA), ``finalize`` records one whole fixed step with
``wp.ScopedCapture`` and ``step`` replays it with ``wp.capture_launch``. A CUDA
graph records kernel launches bound to specific GPU buffers; it does NOT record
the Python-level reference swap. So the recording must end with the newest
state in the same buffer it started from: with an even substep count the swaps
cancel out, and with an odd count the last result is copied back into
``state_0`` (``State.assign``, a recorded device copy). Either way
``self._state_0`` is always the current state, between steps, with or without a
graph. Every other buffer a graph touches (wrench buffer, kinematic buffers,
contacts, solver temporaries) is allocated once and only ever written in place
(``wp.copy``, ``zero_``, kernels) - never rebound.

Warp allocations made while recording (XPBD allocates temporaries inside
``step``) come from the CUDA memory pool and are kept alive by the graph, which
is why capture needs the Warp mempool (on by default on CUDA).

``finalize`` runs one fixed step to compile every kernel, records the graph
(which runs a second step), then copies the snapshot of the initial state back
into both state buffers, so the first ``step`` has no compile hitch and starts
exactly from the authored poses and velocities.

Host transfers: ``read_body_poses`` copies ``body_q`` into a preallocated
pinned host buffer and then into the caller's array. Both directions are
asynchronous - ``wp.copy`` only enqueues a ``cudaMemcpyAsync`` on the device's
stream - and pinned memory is the case where that is actually observable, so
the bridge synchronizes explicitly: a readback waits on the stream before
reading the numpy view (which also waits for the fixed steps queued ahead of
it), and an upload records a CUDA event that is waited on before its staging
buffer is rewritten. ``step`` and ``read_body_poses`` allocate nothing.

Kinematic bodies
----------------
Kinematic bodies carry Newton's ``BodyFlags.KINEMATIC``. XPBD gives them zero
effective inverse mass and passes their pose and velocity through the solve
unchanged (``copy_kinematic_body_state``), so the application must write their
pose before every substep. ``set_kinematic_poses`` records a target; the next
``step(n)`` call uploads the start pose and the target once, and a small kernel
launched inside the recorded substep writes, for substep ``k`` of the
``T = n * substeps`` substeps in the call::

    alpha    = (k + 1) / T                 (a device-side counter, so the graph
                                            replays correctly for any n)
    position = start + (target - start) * alpha
    rotation = slerp along the shortest arc from start to target (world frame)
    body_qd  = constant linear and angular velocity of that motion

At ``alpha = 1`` the pose is set to the target exactly. Contacts are generated
against the already advanced kinematic pose, and XPBD's positional contact
projection pushes dynamic bodies out of the way; the prescribed velocity feeds
XPBD's friction, so a moving kinematic surface also drags what rests on it. A
kinematic body with no new target holds still (zero velocity). This needs no
joints and works for free-floating kinematic bodies.

Solvers
-------
* ``xpbd`` (default, fully supported and the only one this bridge is tuned for):
  ``SolverXPBD(iterations=solver_iterations)``. Restitution in XPBD is a separate
  velocity pass that Newton only runs with ``enable_restitution=True``; the
  bridge enables it when any collider has a restitution above zero.

  Both ``mu`` and ``restitution`` are the ARITHMETIC MEAN of the two touching
  shapes (``mat_nonzero`` in Newton's XPBD kernels). A bouncy sphere on a dull
  floor therefore bounces with ``(0.8 + 0.0) / 2 = 0.4``: to get a given
  restitution, author it on BOTH surfaces.
* ``featherstone`` and ``semi_implicit`` (best effort, measured): both simulate,
  and kinematic bodies are driven and do push dynamic bodies. They resolve
  contacts with penalty springs (``ShapeConfig.ke/kd/kf``), which the contract
  does not expose, so a body rests a few millimetres inside a surface (a
  radius-0.5 sphere settles at y = 0.4961 instead of 0.5000) and the push of a
  kinematic body overshoots rather than stopping at contact. ``solver_iterations``
  is unused by both. Featherstone integrates joint coordinates, so the bridge
  also refreshes the free joints' ``joint_q/joint_qd`` from ``body_q/body_qd``
  (``newton.eval_ik``) whenever it writes body state.
* ``mujoco``: needs the ``mujoco`` and ``mujoco_warp`` packages, which are NOT
  installed in this environment, so ``finalize`` raises a RuntimeError naming
  them. Everything else about the bridge is solver-independent, so installing
  those two packages is all this path needs.

Tuning and cost (measured on an RTX 3070, Newton 1.6.0 / Warp 1.17.0)
---------------------------------------------------------------------
``substeps = 10``, ``solver_iterations = 2`` (the C++ defaults) are a good
choice at 60 Hz and need no change. A five-box stack stands for 5 s with the
top box drifting under 0.05 mm at every setting from 4 substeps up; what
substeps buy is less contact penetration, which is what a stack looks wrong
from: the bottom box sinks 3.74 mm at 4 substeps, 0.93 mm at 8, 0.59 mm at 10
and 0.23 mm at 16. Ten substeps is the knee - under a millimetre, and cheap.
``solver_iterations = 4`` halves the sink again (0.29 mm) for a few percent
more time, so it is the first knob to turn if stacks look soft.

Cost scales with substeps, and the CUDA graph is what makes that affordable:
60 mixed bodies on a plane at 10 substeps cost **2.68 ms** per fixed step with
the graph and **41.6 ms** without it (15.5x), because a fixed step is ~10
substeps x (collide + solve) x many small kernels, and per-launch overhead
dominates at this scene size. Always leave ``use_cuda_graph`` on. A warm
``finalize`` of that scene takes about 0.4 s; the first ever run on a machine
compiles Warp kernels and takes minutes (~18 s for a small scene here, after
Newton's own kernels were already cached).

Contract deviations
-------------------
None in behavior. Clarifications of points the contract leaves open:

* Linear velocities (``add_body``, ``set_body_velocities``, impulses) are
  center-of-mass velocities, as in Newton.
* ``step(0)`` is a no-op: pending wrenches and kinematic targets stay queued for
  the next non-empty step, and ``previous_poses_out`` (if given) receives the
  current poses.
* ``set_body_velocities`` ignores kinematic bodies, like ``apply_impulses``: a
  kinematic body's velocity is defined by its targets.
* Calling ``set_wrenches`` twice before ``step`` replaces the buffer; impulses
  for the same body within one ``apply_impulses`` call add up; for repeated
  indices in ``set_body_velocities``/``set_kinematic_poses`` one entry wins.
* "The next ``step`` call" means the CALL, not one fixed step - a wrench set
  once covers all ``num_steps`` fixed steps of ``step(num_steps)`` and is then
  cleared. So do ``set_wrenches(...)`` then one ``step(n)``, as the frame loop
  above does. ``set_wrenches(...)`` followed by ``n`` separate ``step(1)`` calls
  applies the force during the first one only. The same holds for a kinematic
  target: it is reached at the end of the call, so a target plus ``step(4)``
  sweeps there over four fixed steps, not four times over.
* A plane must be static (``body = -1``), and every body needs at least one
  collider.
* ``body_count()`` and ``description()`` still answer after ``close()``; every
  other call raises.
"""

from __future__ import annotations

import gc
import math
import operator
import warnings

import numpy as np
import warp as wp

import newton
from newton.geometry import compute_inertia_shape

__all__ = ["BRIDGE_VERSION", "World"]

BRIDGE_VERSION = 1

# Enum numbers shared with the C++ enums RigidbodyType and ColliderShape.
BODY_DYNAMIC = 0
BODY_KINEMATIC = 1

SHAPE_SPHERE = 0
SHAPE_BOX = 1
SHAPE_CAPSULE = 2
SHAPE_PLANE = 3

_SHAPE_NAMES = {SHAPE_SPHERE: "sphere", SHAPE_BOX: "box", SHAPE_CAPSULE: "capsule", SHAPE_PLANE: "plane"}

_SOLVER_LABELS = {
    "xpbd": "XPBD",
    "mujoco": "MuJoCo",
    "featherstone": "Featherstone",
    "semi_implicit": "SemiImplicit",
}

# The PhysicsSettings defaults on the C++ side, used for keys a caller leaves out.
_DEFAULT_SETTINGS = {
    "gravity": (0.0, -9.81, 0.0),
    "fixed_dt": 1.0 / 60.0,
    "substeps": 10,
    "solver": "xpbd",
    "solver_iterations": 2,
    "device": "cuda",
    "use_cuda_graph": True,
}

# Density for kinematic bodies' shapes. XPBD ignores it (kinematic bodies get
# zero effective inverse mass); reduced-coordinate solvers just need something sane.
_KINEMATIC_DENSITY = 1000.0

# Rotation of -90 degrees about X, (x, y, z, w). It maps local +Z onto local +Y:
# appended to capsules (Newton: axis along Z) and planes (Newton: normal along Z).
_Z_TO_Y = (-math.sqrt(0.5), 0.0, 0.0, math.sqrt(0.5))

_KINEMATIC_FLAG = wp.constant(int(newton.BodyFlags.KINEMATIC))


# --------------------------------------------------------------------------- kernels


@wp.kernel
def _start_kinematic_clock(clock: wp.array[wp.int32], total_substeps: wp.int32):
    """clock[0] = substeps already run in this step() call, clock[1] = substeps in the call."""
    clock[0] = 0
    clock[1] = total_substeps


@wp.kernel
def _advance_kinematic_clock(clock: wp.array[wp.int32]):
    clock[0] = clock[0] + 1


@wp.kernel
def _drive_kinematic_bodies(
    kin_body: wp.array[wp.int32],
    kin_pose: wp.array[wp.transform],  # [0, count): start of the call, [count, 2 count): target
    kin_count: wp.int32,
    clock: wp.array[wp.int32],
    body_com: wp.array[wp.vec3],
    substep_dt: wp.float32,
    body_q: wp.array[wp.transform],
    body_qd: wp.array[wp.spatial_vector],
):
    i = wp.tid()
    b = kin_body[i]
    done = clock[0] + 1
    total = clock[1]

    start = kin_pose[i]
    target = kin_pose[i + kin_count]
    p0 = wp.transform_get_translation(start)
    p1 = wp.transform_get_translation(target)
    q0 = wp.transform_get_rotation(start)
    q1 = wp.transform_get_rotation(target)

    # World-frame rotation from q0 to q1, flipped onto the shortest arc.
    dq = q1 * wp.quat_inverse(q0)
    if dq[3] < 0.0:
        dq = wp.quat(-dq[0], -dq[1], -dq[2], -dq[3])
    s = wp.length(wp.vec3(dq[0], dq[1], dq[2]))
    angle = 2.0 * wp.atan2(s, dq[3])
    axis = wp.vec3(0.0, 0.0, 0.0)
    if s > 1.0e-9:
        axis = wp.vec3(dq[0], dq[1], dq[2]) / s

    duration = float(wp.max(total, 1)) * substep_dt
    lin_vel = (p1 - p0) / duration
    ang_vel = axis * (angle / duration)

    p = p1
    q = q1
    if done < total:
        alpha = float(done) / float(total)
        p = p0 + (p1 - p0) * alpha
        q = wp.quat_from_axis_angle(axis, angle * alpha) * q0

    body_q[b] = wp.transform(p, q)
    # body_qd holds the velocity of the center of mass.
    v_com = lin_vel + wp.cross(ang_vel, wp.quat_rotate(q, body_com[b]))
    body_qd[b] = wp.spatial_vector(v_com, ang_vel)


@wp.kernel
def _apply_linear_impulses(
    indices: wp.array[wp.int32],
    impulses: wp.array[wp.vec3],
    body_inv_mass: wp.array[wp.float32],
    body_flags: wp.array[wp.int32],
    body_qd: wp.array[wp.spatial_vector],
):
    i = wp.tid()
    b = indices[i]
    if (body_flags[b] & _KINEMATIC_FLAG) != 0:
        return
    dv = impulses[i] * body_inv_mass[b]
    # atomic: the same body may appear more than once
    wp.atomic_add(body_qd, b, wp.spatial_vector(dv, wp.vec3(0.0, 0.0, 0.0)))


@wp.kernel
def _set_velocities(
    indices: wp.array[wp.int32],
    linear: wp.array[wp.vec3],
    angular: wp.array[wp.vec3],
    body_flags: wp.array[wp.int32],
    body_qd: wp.array[wp.spatial_vector],
):
    i = wp.tid()
    b = indices[i]
    if (body_flags[b] & _KINEMATIC_FLAG) != 0:
        return
    body_qd[b] = wp.spatial_vector(linear[i], angular[i])


# --------------------------------------------------------------------------- validation helpers


def _integer(value, name, minimum=None):
    if isinstance(value, (bool, np.bool_)):
        raise TypeError(f"{name} must be an integer, got a bool")
    try:
        result = operator.index(value)
    except TypeError:
        raise TypeError(f"{name} must be an integer, got {type(value).__name__} {value!r}") from None
    if minimum is not None and result < minimum:
        raise ValueError(f"{name} must be >= {minimum}, got {result}")
    return result


def _number(value, name):
    if isinstance(value, (bool, np.bool_)):
        raise TypeError(f"{name} must be a number, got a bool")
    try:
        result = float(value)
    except (TypeError, ValueError):
        raise TypeError(f"{name} must be a number, got {type(value).__name__} {value!r}") from None
    if not math.isfinite(result):
        raise ValueError(f"{name} must be finite, got {result}")
    return result


def _numbers(value, count, name):
    if isinstance(value, (str, bytes)):
        raise TypeError(f"{name} must be {count} numbers, got {type(value).__name__}")
    try:
        items = list(value)
    except TypeError:
        raise TypeError(f"{name} must be a sequence of {count} numbers, got {type(value).__name__}") from None
    if len(items) != count:
        raise ValueError(f"{name} must have {count} numbers, got {len(items)}")
    return tuple(_number(v, f"{name}[{k}]") for k, v in enumerate(items))


def _unit_quaternion(value, name):
    x, y, z, w = _numbers(value, 4, name)
    norm = math.sqrt(x * x + y * y + z * z + w * w)
    if norm < 1.0e-6:
        raise ValueError(f"{name} must be a non-zero quaternion (x, y, z, w), got {(x, y, z, w)}")
    return (x / norm, y / norm, z / norm, w / norm)


def _quat_mul(a, b):
    """Hamilton product a * b of (x, y, z, w) quaternions: rotate by b, then by a."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw * bx + bw * ax + ay * bz - az * by,
        aw * by + bw * ay + az * bx - ax * bz,
        aw * bz + bw * az + ax * by - ay * bx,
        aw * bw - ax * bx - ay * by - az * bz,
    )


def _array(value, dtype, shape, name, writable=False):
    """Checks a numpy array's type, dtype and shape without copying it."""
    expected = f"a numpy.ndarray with dtype {np.dtype(dtype).name} and shape {shape}"
    if not isinstance(value, np.ndarray):
        raise TypeError(f"{name} must be {expected}, got {type(value).__name__}")
    if value.dtype != dtype:
        raise TypeError(f"{name} must be {expected}, got dtype {value.dtype}")
    if value.shape != shape:
        raise ValueError(f"{name} must be {expected}, got shape {value.shape}")
    if writable and not value.flags.writeable:
        raise ValueError(f"{name} must be writable")
    return value


def _finite(value, name):
    if not np.isfinite(value).all():
        raise ValueError(f"{name} contains NaN or infinite values")


def _normalized_poses(poses, name):
    """Returns a float32 copy of (K, 7) poses with unit quaternions."""
    _finite(poses, name)
    result = np.array(poses, dtype=np.float32, copy=True)
    norms = np.sqrt(np.einsum("ij,ij->i", result[:, 3:7].astype(np.float64), result[:, 3:7].astype(np.float64)))
    if len(norms) and norms.min() < 1.0e-6:
        bad = int(np.argmin(norms))
        raise ValueError(f"{name}[{bad}] has a zero quaternion; rotations are (qx, qy, qz, qw)")
    result[:, 3:7] = result[:, 3:7] / norms[:, None].astype(np.float32)
    return result


class _Body:
    __slots__ = ("kind", "position", "rotation", "mass", "linear_velocity", "angular_velocity")

    def __init__(self, kind, position, rotation, mass, linear_velocity, angular_velocity):
        self.kind = kind
        self.position = position
        self.rotation = rotation
        self.mass = mass
        self.linear_velocity = linear_velocity
        self.angular_velocity = angular_velocity


class _Collider:
    __slots__ = (
        "body",
        "shape",
        "position",
        "rotation",
        "half_extent",
        "radius",
        "half_height",
        "friction",
        "restitution",
        "volume",
    )

    def __init__(self, **fields):
        for key, value in fields.items():
            setattr(self, key, value)


# --------------------------------------------------------------------------- World


class World:
    """One Newton simulation: build it, finalize it, step it, read poses, close it.

    All methods must be called from one thread. Arrays passed in may be views of
    C++ memory; they are read or filled in place and never kept.
    """

    def __init__(self, settings: dict):
        """Creates an empty world.

        ``settings`` keys (missing keys take the C++ ``PhysicsSettings`` defaults,
        unknown keys raise):

        ============== ======= =====================================================
        gravity        3 floats m/s^2, world space
        fixed_dt       float   simulated seconds per fixed step (> 0)
        substeps       int     solver substeps per fixed step (>= 1)
        solver         str     "xpbd", "mujoco", "featherstone" or "semi_implicit"
        solver_iterations int  iterations for the solvers that iterate (>= 1)
        device         str     "cuda" (first GPU; CPU with a warning if none) or "cpu"
        use_cuda_graph bool    record one fixed step as a CUDA graph (CUDA only)
        ============== ======= =====================================================

        Nothing is allocated on the device until :meth:`finalize`.
        """
        self._closed = False
        self._finalized = False
        self._finalize_attempted = False
        self._graph = None
        self._model = None

        if not isinstance(settings, dict):
            raise TypeError(f"settings must be a dict, got {type(settings).__name__}")
        unknown = sorted(set(settings) - set(_DEFAULT_SETTINGS))
        if unknown:
            raise ValueError(f"unknown settings key(s) {unknown}; expected keys are {sorted(_DEFAULT_SETTINGS)}")
        merged = dict(_DEFAULT_SETTINGS)
        merged.update(settings)

        self._gravity = _numbers(merged["gravity"], 3, "settings['gravity']")
        self._fixed_dt = _number(merged["fixed_dt"], "settings['fixed_dt']")
        if self._fixed_dt <= 0.0:
            raise ValueError(f"settings['fixed_dt'] must be > 0, got {self._fixed_dt}")
        self._substeps = _integer(merged["substeps"], "settings['substeps']", minimum=1)
        self._substep_dt = self._fixed_dt / self._substeps
        self._iterations = _integer(merged["solver_iterations"], "settings['solver_iterations']", minimum=1)

        solver = merged["solver"]
        if not isinstance(solver, str) or solver not in _SOLVER_LABELS:
            raise ValueError(f"settings['solver'] must be one of {sorted(_SOLVER_LABELS)}, got {solver!r}")
        self._solver_name = solver
        # Reduced-coordinate solvers integrate joint_q/joint_qd, so body-level writes
        # must be mirrored into the free joints' coordinates.
        self._uses_joint_coordinates = solver in ("featherstone", "mujoco")

        use_graph = merged["use_cuda_graph"]
        if not isinstance(use_graph, (bool, np.bool_)):
            raise TypeError(f"settings['use_cuda_graph'] must be a bool, got {type(use_graph).__name__}")
        self._want_graph = bool(use_graph)

        device = merged["device"]
        if not isinstance(device, str) or not (device in ("cuda", "cpu") or device.startswith("cuda:")):
            raise ValueError(f"settings['device'] must be 'cuda' or 'cpu', got {device!r}")
        if device == "cpu":
            self._device = wp.get_device("cpu")
        elif wp.is_cuda_available():
            name = "cuda:0" if device == "cuda" else device
            if not wp.is_device_available(wp.get_device(name)):
                raise ValueError(f"settings['device'] names a CUDA device that does not exist: {device!r}")
            self._device = wp.get_device(name)
        else:
            warnings.warn(
                "aviator_newton: no CUDA device is available, so Newton runs on the CPU",
                RuntimeWarning,
                stacklevel=2,
            )
            self._device = wp.get_device("cpu")

        self._bodies: list[_Body] = []
        self._colliders: list[_Collider] = []

    # ------------------------------------------------------------------ context manager

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()

    # ------------------------------------------------------------------ building

    def add_body(self, kind, position, rotation, mass, linear_velocity, angular_velocity) -> int:
        """Records a rigid body and returns its index (0, 1, 2, ... in call order).

        Args:
            kind: 0 dynamic, 1 kinematic.
            position: 3 floats, world-space position of the body origin.
            rotation: 4 floats (x, y, z, w); normalized here.
            mass: kg, > 0 for a dynamic body; spread over the body's colliders by
                volume in :meth:`finalize`. Ignored for a kinematic body.
            linear_velocity: 3 floats, world-space start velocity of the center of
                mass (m/s). Ignored for a kinematic body, which starts at rest.
            angular_velocity: 3 floats, world-space start angular velocity (rad/s).
                Ignored for a kinematic body.
        """
        self._check_building("add_body")
        kind = _integer(kind, "kind")
        if kind not in (BODY_DYNAMIC, BODY_KINEMATIC):
            raise ValueError(f"kind must be 0 (dynamic) or 1 (kinematic), got {kind}")
        position = _numbers(position, 3, "position")
        rotation = _unit_quaternion(rotation, "rotation")
        if kind == BODY_DYNAMIC:
            mass = _number(mass, "mass")
            if mass <= 0.0:
                raise ValueError(f"mass of a dynamic body must be > 0 kg, got {mass}")
            linear_velocity = _numbers(linear_velocity, 3, "linear_velocity")
            angular_velocity = _numbers(angular_velocity, 3, "angular_velocity")
        else:
            mass = 0.0
            linear_velocity = (0.0, 0.0, 0.0)
            angular_velocity = (0.0, 0.0, 0.0)
        self._bodies.append(_Body(kind, position, rotation, mass, linear_velocity, angular_velocity))
        return len(self._bodies) - 1

    def add_collider(
        self,
        body,
        shape,
        local_position,
        local_rotation,
        half_extent,
        radius,
        half_height,
        friction,
        restitution,
    ) -> int:
        """Records a collision shape and returns its index (in call order).

        Args:
            body: index returned by :meth:`add_body`, or -1 for static geometry
                (the local pose is then a world pose).
            shape: 0 sphere (``radius``), 1 box (``half_extent``), 2 capsule
                (``radius`` and ``half_height``, the straight segment along local Y),
                3 plane (infinite, normal along local +Y, static only).
            local_position: 3 floats, shape pose relative to the body origin.
            local_rotation: 4 floats (x, y, z, w).
            half_extent: 3 floats, box half sizes (> 0 for boxes).
            radius: sphere/capsule radius (> 0 for those shapes).
            half_height: half the capsule's straight segment (>= 0 for capsules).
            friction: Coulomb coefficient (>= 0). Newton averages the two surfaces.
            restitution: 0 (no bounce) to 1 (elastic). Newton averages the two surfaces.

        Arguments a shape does not use are accepted and ignored.
        """
        self._check_building("add_collider")
        body = _integer(body, "body")
        if body < -1 or body >= len(self._bodies):
            raise IndexError(f"body must be -1 (static) or a body index in [0, {len(self._bodies)}), got {body}")
        shape = _integer(shape, "shape")
        if shape not in _SHAPE_NAMES:
            raise ValueError(f"shape must be 0 sphere, 1 box, 2 capsule or 3 plane, got {shape}")
        position = _numbers(local_position, 3, "local_position")
        rotation = _unit_quaternion(local_rotation, "local_rotation")
        half_extent = _numbers(half_extent, 3, "half_extent")
        radius = _number(radius, "radius")
        half_height = _number(half_height, "half_height")
        friction = _number(friction, "friction")
        restitution = _number(restitution, "restitution")
        if friction < 0.0:
            raise ValueError(f"friction must be >= 0, got {friction}")
        if not 0.0 <= restitution <= 1.0:
            raise ValueError(f"restitution must be in [0, 1], got {restitution}")

        if shape == SHAPE_SPHERE:
            if radius <= 0.0:
                raise ValueError(f"a sphere needs radius > 0, got {radius}")
            scale = (radius, 0.0, 0.0)
            geo = newton.GeoType.SPHERE
        elif shape == SHAPE_BOX:
            if min(half_extent) <= 0.0:
                raise ValueError(f"a box needs every half_extent > 0, got {half_extent}")
            scale = half_extent
            geo = newton.GeoType.BOX
        elif shape == SHAPE_CAPSULE:
            if radius <= 0.0:
                raise ValueError(f"a capsule needs radius > 0, got {radius}")
            if half_height < 0.0:
                raise ValueError(f"a capsule needs half_height >= 0, got {half_height}")
            scale = (radius, half_height, 0.0)
            geo = newton.GeoType.CAPSULE
        else:
            if body != -1:
                raise ValueError("a plane collider must be static geometry (body = -1)")
            scale = None
            geo = newton.GeoType.PLANE

        volume = 0.0
        if scale is not None:
            volume = float(compute_inertia_shape(geo, wp.vec3(*scale), None, 1.0)[0])

        self._colliders.append(
            _Collider(
                body=body,
                shape=shape,
                position=position,
                rotation=rotation,
                half_extent=half_extent,
                radius=radius,
                half_height=half_height,
                friction=friction,
                restitution=restitution,
                volume=volume,
            )
        )
        return len(self._colliders) - 1

    def finalize(self) -> None:
        """Builds the Newton model, solver, collision pipeline and every buffer.

        Spreads each dynamic body's mass over its colliders by volume, applies
        the start velocities, compiles all kernels by running one fixed step,
        records the CUDA graph (if enabled), and finally restores the initial
        poses and velocities exactly. The first call in a fresh Warp kernel cache
        compiles kernels and can take minutes; later runs load them from disk.
        """
        self._check_open()
        if self._finalize_attempted:
            raise RuntimeError("finalize() was already called; build a new World to change the scene")
        self._finalize_attempted = True

        body_count = len(self._bodies)
        collider_count = [0] * body_count
        body_volume = [0.0] * body_count
        for collider in self._colliders:
            if collider.body >= 0:
                collider_count[collider.body] += 1
                body_volume[collider.body] += collider.volume
        for index, count in enumerate(collider_count):
            if count == 0:
                raise ValueError(f"body {index} has no collider; every body needs at least one")

        self._body_count = body_count
        if body_count == 0:
            # Nothing can move; there is nothing to simulate.
            self._finalized = True
            return

        device = self._device
        self._is_cuda = device.is_cuda

        # ---- model
        builder = newton.ModelBuilder(up_axis=newton.Axis.Y, gravity=self._gravity)
        for index, body in enumerate(self._bodies):
            created = builder.add_body(
                xform=wp.transform(wp.vec3(*body.position), wp.quat(*body.rotation)),
                is_kinematic=body.kind == BODY_KINEMATIC,
            )
            if created != index:
                raise RuntimeError(f"Newton numbered body {index} as {created}")

        any_restitution = False
        for collider in self._colliders:
            if collider.body < 0:
                density = 0.0
            elif self._bodies[collider.body].kind == BODY_KINEMATIC:
                density = _KINEMATIC_DENSITY
            else:
                density = self._bodies[collider.body].mass / body_volume[collider.body]
            any_restitution = any_restitution or collider.restitution > 0.0
            cfg = newton.ModelBuilder.ShapeConfig(
                density=density,
                mu=collider.friction,
                restitution=collider.restitution,
            )
            rotation = collider.rotation
            if collider.shape in (SHAPE_CAPSULE, SHAPE_PLANE):
                rotation = _quat_mul(rotation, _Z_TO_Y)
            xform = wp.transform(wp.vec3(*collider.position), wp.quat(*rotation))
            if collider.shape == SHAPE_SPHERE:
                builder.add_shape_sphere(collider.body, xform=xform, radius=collider.radius, cfg=cfg)
            elif collider.shape == SHAPE_BOX:
                hx, hy, hz = collider.half_extent
                builder.add_shape_box(collider.body, xform=xform, hx=hx, hy=hy, hz=hz, cfg=cfg)
            elif collider.shape == SHAPE_CAPSULE:
                builder.add_shape_capsule(
                    collider.body,
                    xform=xform,
                    radius=collider.radius,
                    half_height=collider.half_height,
                    cfg=cfg,
                )
            else:
                builder.add_shape_plane(xform=xform, width=0.0, length=0.0, body=-1, cfg=cfg)

        model = builder.finalize(device=device)
        self._model = model

        # ---- collision pipeline (before the solver: it sizes model.rigid_contact_max)
        self._pipeline = newton.CollisionPipeline(
            model,
            broad_phase=self._broad_phase_for(model),
            include_static_kinematic_pairs=False,
        )
        self._contacts = self._pipeline.contacts()

        # ---- solver
        self._solver = self._create_solver(model, any_restitution)

        # ---- state
        self._state_0 = model.state()
        self._state_1 = model.state()
        self._control = model.control()

        start_velocity = np.zeros((body_count, 6), dtype=np.float32)
        for index, body in enumerate(self._bodies):
            start_velocity[index, 0:3] = body.linear_velocity
            start_velocity[index, 3:6] = body.angular_velocity
        self._state_0.body_qd.assign(start_velocity)
        self._sync_joint_coordinates(self._state_0)
        self._state_1.assign(self._state_0)
        self._initial_state = [
            (name, wp.clone(getattr(self._state_0, name)))
            for name in ("body_q", "body_qd", "joint_q", "joint_qd")
            if getattr(self._state_0, name) is not None
        ]

        # ---- buffers (allocated once; step/read_body_poses allocate nothing)
        pinned = self._is_cuda
        n = body_count
        self._wrench = wp.zeros(n, dtype=wp.spatial_vector, device=device)
        self._wrench_pending = False
        self._wrench_stage = wp.empty(n, dtype=wp.spatial_vector, device="cpu", pinned=pinned)
        self._wrench_stage_np = self._wrench_stage.numpy()
        self._pose_readback = wp.empty(n, dtype=wp.transform, device="cpu", pinned=pinned)
        self._pose_readback_np = self._pose_readback.numpy()
        self._previous_pose = wp.empty(n, dtype=wp.transform, device=device)
        self._previous_readback = wp.empty(n, dtype=wp.transform, device="cpu", pinned=pinned)
        self._previous_readback_np = self._previous_readback.numpy()
        self._pose_stage = wp.empty(n, dtype=wp.transform, device="cpu", pinned=pinned)
        self._pose_stage_np = self._pose_stage.numpy()
        self._scratch_capacity = 0
        self._ensure_scratch(n)

        self._upload_event = wp.Event(device) if self._is_cuda else None
        self._upload_pending = False

        # ---- kinematic bodies
        kinematic = [i for i, body in enumerate(self._bodies) if body.kind == BODY_KINEMATIC]
        self._kin_count = len(kinematic)
        self._kin_slot_of_body = np.full(n, -1, dtype=np.int32)
        self._kin_dirty = False
        self._kin_moving = False
        if kinematic:
            self._kin_slot_of_body[kinematic] = np.arange(len(kinematic), dtype=np.int32)
            self._kin_body = wp.array(np.asarray(kinematic, dtype=np.int32), dtype=wp.int32, device=device)
            self._kin_current = np.asarray(
                [self._bodies[i].position + self._bodies[i].rotation for i in kinematic], dtype=np.float32
            )
            self._kin_target = self._kin_current.copy()
            self._kin_stage = wp.empty(2 * len(kinematic), dtype=wp.transform, device="cpu", pinned=pinned)
            self._kin_stage_np = self._kin_stage.numpy()
            self._kin_pose = wp.empty(2 * len(kinematic), dtype=wp.transform, device=device)
            self._kin_clock = wp.zeros(2, dtype=wp.int32, device=device)
            self._upload_kinematic_motion()

        # ---- warm-up: compile every kernel, then record the graph
        self._start_clock(1)
        self._simulate_fixed_step()
        self._use_graph = False
        if self._want_graph and self._is_cuda:
            if wp.is_mempool_enabled(device):
                self._start_clock(1)
                with wp.ScopedCapture(device=device) as capture:
                    self._simulate_fixed_step()
                self._graph = capture.graph
                self._use_graph = True
            else:
                warnings.warn(
                    f"aviator_newton: the Warp memory pool is disabled on {device}, so no CUDA graph is recorded",
                    RuntimeWarning,
                    stacklevel=2,
                )

        # ---- back to the authored initial state
        self._restore_initial_state()
        wp.synchronize_device(device)
        self._finalized = True

    # ------------------------------------------------------------------ simulating

    def step(self, num_steps, previous_poses_out=None) -> None:
        """Advances ``num_steps`` fixed steps.

        If ``previous_poses_out`` (float32 ``(N, 7)``) is given, it is filled with
        the poses from before the final fixed step, for interpolation; with
        ``num_steps == 1`` those are the poses at the start of the call. Wrenches
        from :meth:`set_wrenches` act on every substep of this call and are then
        cleared; kinematic targets are reached at the end of this call.
        ``num_steps == 0`` does nothing (queued inputs stay queued).
        """
        self._check_finalized("step")
        num_steps = _integer(num_steps, "num_steps", minimum=0)
        if previous_poses_out is not None:
            _array(previous_poses_out, np.float32, (self._body_count, 7), "previous_poses_out", writable=True)
        if self._body_count == 0:
            return
        if num_steps == 0:
            if previous_poses_out is not None:
                self.read_body_poses(previous_poses_out)
            return

        if self._kin_count:
            if self._kin_dirty or self._kin_moving:
                self._upload_kinematic_motion()
            self._start_clock(num_steps)

        last = num_steps - 1
        for index in range(num_steps):
            if index == last and previous_poses_out is not None:
                wp.copy(self._previous_pose, self._state_0.body_q)
            if self._use_graph:
                wp.capture_launch(self._graph)
            else:
                self._simulate_fixed_step()

        if previous_poses_out is not None:
            self._download(self._previous_readback, self._previous_pose)
            np.copyto(previous_poses_out, self._previous_readback_np)

        # one-shot inputs are consumed by this call
        if self._wrench_pending:
            self._wrench.zero_()
            self._wrench_pending = False
        if self._kin_count:
            np.copyto(self._kin_current, self._kin_target)
            self._kin_moving = self._kin_dirty
            self._kin_dirty = False

    def read_body_poses(self, out) -> None:
        """Fills ``out`` (float32 ``(N, 7)``) with the current body poses."""
        self._check_finalized("read_body_poses")
        _array(out, np.float32, (self._body_count, 7), "out", writable=True)
        if self._body_count == 0:
            return
        self._download(self._pose_readback, self._state_0.body_q)
        np.copyto(out, self._pose_readback_np)

    def write_body_poses(self, poses) -> None:
        """Teleports every body to ``poses`` (float32 ``(N, 7)``).

        Zeroes every velocity and the pending wrenches, and cancels kinematic
        targets (kinematic bodies hold still at their new pose).
        """
        self._check_finalized("write_body_poses")
        n = self._body_count
        _array(poses, np.float32, (n, 7), "poses")
        if n == 0:
            return
        normalized = _normalized_poses(poses, "poses")

        self._wait_for_uploads()
        np.copyto(self._pose_stage_np, normalized)
        for state in (self._state_0, self._state_1):
            wp.copy(state.body_q, self._pose_stage)
        self._note_upload()
        for state in (self._state_0, self._state_1):
            state.body_qd.zero_()
            state.body_f.zero_()
            self._sync_joint_coordinates(state)
        self._wrench.zero_()
        self._wrench_pending = False

        if self._kin_count:
            kinematic_poses = normalized[self._kin_slot_of_body >= 0]
            np.copyto(self._kin_current, kinematic_poses)
            np.copyto(self._kin_target, kinematic_poses)
            self._kin_dirty = True

    def set_body_velocities(self, indices, linear, angular) -> None:
        """Sets the world-space velocities of the bodies in ``indices``.

        ``indices`` int32 ``(K,)``; ``linear`` and ``angular`` float32 ``(K, 3)``.
        ``linear`` is the velocity of the center of mass. Kinematic bodies are
        ignored (their velocity comes from their targets).
        """
        self._check_finalized("set_body_velocities")
        indices = self._indices(indices, "indices")
        k = indices.shape[0]
        _array(linear, np.float32, (k, 3), "linear")
        _array(angular, np.float32, (k, 3), "angular")
        if k == 0:
            return
        _finite(linear, "linear")
        _finite(angular, "angular")
        self._upload_scratch(indices, linear, angular)
        wp.launch(
            _set_velocities,
            dim=k,
            inputs=[self._scratch_index, self._scratch_a, self._scratch_b, self._model.body_flags],
            outputs=[self._state_0.body_qd],
            device=self._device,
        )
        self._sync_joint_coordinates(self._state_0)

    def apply_impulses(self, indices, impulses) -> None:
        """Linear velocity += impulse * inverse mass, for the bodies in ``indices``.

        ``indices`` int32 ``(K,)``, ``impulses`` float32 ``(K, 3)`` in N*s, world
        space, through the center of mass. Uses the model's per-body inverse mass.
        Kinematic bodies are ignored. Repeated indices add up.
        """
        self._check_finalized("apply_impulses")
        indices = self._indices(indices, "indices")
        k = indices.shape[0]
        _array(impulses, np.float32, (k, 3), "impulses")
        if k == 0:
            return
        _finite(impulses, "impulses")
        self._upload_scratch(indices, impulses, None)
        wp.launch(
            _apply_linear_impulses,
            dim=k,
            inputs=[self._scratch_index, self._scratch_a, self._model.body_inv_mass, self._model.body_flags],
            outputs=[self._state_0.body_qd],
            device=self._device,
        )
        self._sync_joint_coordinates(self._state_0)

    def set_wrenches(self, wrenches) -> None:
        """Sets the external wrench of every body for the next :meth:`step` call.

        ``wrenches`` float32 ``(N, 6)``: ``fx, fy, fz, tx, ty, tz``, world space,
        about the center of mass. The buffer acts on every substep of the next
        ``step`` call and is zeroed after it. Calling this again before ``step``
        replaces the buffer. Kinematic bodies ignore forces.
        """
        self._check_finalized("set_wrenches")
        n = self._body_count
        _array(wrenches, np.float32, (n, 6), "wrenches")
        if n == 0:
            return
        _finite(wrenches, "wrenches")
        self._wait_for_uploads()
        np.copyto(self._wrench_stage_np, wrenches)
        wp.copy(self._wrench, self._wrench_stage)
        self._note_upload()
        self._wrench_pending = True

    def set_kinematic_poses(self, indices, poses) -> None:
        """Sets where kinematic bodies must be at the end of the next :meth:`step` call.

        ``indices`` int32 ``(K,)`` of kinematic bodies, ``poses`` float32 ``(K, 7)``.
        They move there at constant linear and angular velocity (shortest arc),
        pushing dynamic bodies out of the way. Kinematic bodies without a new
        target hold still. Indices of dynamic bodies raise.
        """
        self._check_finalized("set_kinematic_poses")
        indices = self._indices(indices, "indices")
        k = indices.shape[0]
        _array(poses, np.float32, (k, 7), "poses")
        if k == 0:
            return
        slots = self._kin_slot_of_body[indices]
        if (slots < 0).any():
            bad = int(indices[int(np.argmax(slots < 0))])
            raise ValueError(f"body {bad} is not kinematic; set_kinematic_poses only moves kinematic bodies")
        self._kin_target[slots] = _normalized_poses(poses, "poses")
        self._kin_dirty = True

    # ------------------------------------------------------------------ info and teardown

    def body_count(self) -> int:
        """Number of :meth:`add_body` calls so far; body ``i`` is the ``i``-th call."""
        return len(self._bodies)

    def description(self) -> str:
        """E.g. ``"Newton 1.6.0 / Warp 1.17.0 (XPBD, cuda:0, CUDA graph)"``."""
        details = [_SOLVER_LABELS[self._solver_name], str(self._device)]
        if self._graph is not None:
            details.append("CUDA graph")
        if not self._finalized:
            details.append("not finalized")
        elif self._closed:
            details.append("closed")
        return f"Newton {newton.__version__} / Warp {wp.__version__} ({', '.join(details)})"

    def close(self) -> None:
        """Releases the CUDA graph, solver, model and every buffer. Safe to call twice."""
        if self._closed:
            return
        self._closed = True
        if self._model is not None and self._device.is_cuda:
            try:
                wp.synchronize_device(self._device)
            except Exception:  # noqa: BLE001 - teardown must not throw
                pass
        self._graph = None
        # Drop the Newton objects and every buffer. Named exactly, not by
        # prefix: _solver_name and _device must survive for description().
        for name in ("_state_0", "_state_1", "_control", "_initial_state", "_solver", "_pipeline",
                     "_contacts", "_model"):
            if hasattr(self, name):
                setattr(self, name, None)
        for name in list(vars(self)):
            if isinstance(getattr(self, name), (wp.array, wp.Event, np.ndarray)):
                setattr(self, name, None)
        gc.collect()

    # ------------------------------------------------------------------ internals

    def _check_open(self):
        if self._closed:
            raise RuntimeError("this World is closed")

    def _check_building(self, method):
        self._check_open()
        if self._finalize_attempted:
            raise RuntimeError(f"{method}() must be called before finalize()")

    def _check_finalized(self, method):
        self._check_open()
        if not self._finalized:
            if self._finalize_attempted:
                raise RuntimeError(f"{method}(): finalize() failed, so this World cannot simulate")
            raise RuntimeError(f"{method}() must be called after finalize()")

    def _indices(self, indices, name):
        if not isinstance(indices, np.ndarray) or indices.dtype != np.int32 or indices.ndim != 1:
            got = f"dtype {indices.dtype} and shape {indices.shape}" if isinstance(indices, np.ndarray) else type(indices).__name__
            raise TypeError(f"{name} must be a numpy.ndarray with dtype int32 and shape (K,), got {got}")
        if indices.size and (indices.min() < 0 or indices.max() >= self._body_count):
            bad = int(indices[(indices < 0) | (indices >= self._body_count)][0])
            raise IndexError(f"{name} contains {bad}, outside the body range [0, {self._body_count})")
        return indices

    def _broad_phase_for(self, model):
        # "explicit" tests the pairs Newton precomputed at finalize (all shape
        # pairs that may collide); sweep-and-prune scales better once there are
        # many of them.
        pairs = int(getattr(model, "shape_contact_pair_count", 0) or 0)
        return "explicit" if pairs <= _EXPLICIT_BROAD_PHASE_MAX_PAIRS else "sap"

    def _create_solver(self, model, any_restitution):
        solvers = newton.solvers
        if self._solver_name == "xpbd":
            return solvers.SolverXPBD(model, iterations=self._iterations, enable_restitution=any_restitution)
        if self._solver_name == "featherstone":
            return solvers.SolverFeatherstone(model)
        if self._solver_name == "semi_implicit":
            return solvers.SolverSemiImplicit(model)
        try:
            solvers.SolverMuJoCo.import_mujoco()
        except ImportError as error:
            raise RuntimeError(
                "solver 'mujoco' needs the 'mujoco' and 'mujoco_warp' packages, which are not installed "
                f"in this Python environment ({error})"
            ) from error
        return solvers.SolverMuJoCo(model, iterations=self._iterations, use_mujoco_contacts=False)

    def _sync_joint_coordinates(self, state):
        """Mirrors body_q/body_qd into the free joints' coordinates for reduced-coordinate solvers."""
        if self._uses_joint_coordinates and state.joint_q is not None:
            newton.eval_ik(self._model, state, state.joint_q, state.joint_qd)

    def _simulate_fixed_step(self):
        """One fixed step. Recorded as the CUDA graph, so: device work only, fixed buffers."""
        s_in = self._state_0
        s_out = self._state_1
        model = self._model
        for _ in range(self._substeps):
            # clear forces + add the external wrench, in one device copy
            wp.copy(s_in.body_f, self._wrench)
            if self._kin_count:
                wp.launch(
                    _drive_kinematic_bodies,
                    dim=self._kin_count,
                    inputs=[
                        self._kin_body,
                        self._kin_pose,
                        self._kin_count,
                        self._kin_clock,
                        model.body_com,
                        self._substep_dt,
                    ],
                    outputs=[s_in.body_q, s_in.body_qd],
                    device=self._device,
                )
                wp.launch(_advance_kinematic_clock, dim=1, inputs=[self._kin_clock], device=self._device)
                if self._uses_joint_coordinates:
                    newton.eval_ik(
                        model, s_in, s_in.joint_q, s_in.joint_qd, body_flag_filter=newton.BodyFlags.KINEMATIC
                    )
            self._pipeline.collide(s_in, self._contacts)
            self._solver.step(s_in, s_out, self._control, self._contacts, self._substep_dt)
            s_in, s_out = s_out, s_in
        if s_in is not self._state_0:
            # odd substep count: copy the newest state back, so it always ends in _state_0
            self._state_0.assign(s_in)

    def _start_clock(self, num_steps):
        if self._kin_count:
            wp.launch(
                _start_kinematic_clock,
                dim=1,
                inputs=[self._kin_clock, num_steps * self._substeps],
                device=self._device,
            )

    def _upload_kinematic_motion(self):
        self._wait_for_uploads()
        k = self._kin_count
        np.copyto(self._kin_stage_np[:k], self._kin_current)
        np.copyto(self._kin_stage_np[k:], self._kin_target)
        wp.copy(self._kin_pose, self._kin_stage)
        self._note_upload()

    def _restore_initial_state(self):
        for name, snapshot in self._initial_state:
            wp.copy(getattr(self._state_0, name), snapshot)
            wp.copy(getattr(self._state_1, name), snapshot)
        self._state_0.body_f.zero_()
        self._state_1.body_f.zero_()
        self._wrench.zero_()
        self._wrench_pending = False

    def _ensure_scratch(self, count):
        if count <= self._scratch_capacity:
            return
        self._wait_for_uploads() if self._scratch_capacity else None
        capacity = max(count, 1)
        pinned = self._is_cuda
        device = self._device
        self._scratch_index_stage = wp.empty(capacity, dtype=wp.int32, device="cpu", pinned=pinned)
        self._scratch_a_stage = wp.empty(capacity, dtype=wp.vec3, device="cpu", pinned=pinned)
        self._scratch_b_stage = wp.empty(capacity, dtype=wp.vec3, device="cpu", pinned=pinned)
        self._scratch_index_stage_np = self._scratch_index_stage.numpy()
        self._scratch_a_stage_np = self._scratch_a_stage.numpy()
        self._scratch_b_stage_np = self._scratch_b_stage.numpy()
        self._scratch_index = wp.empty(capacity, dtype=wp.int32, device=device)
        self._scratch_a = wp.empty(capacity, dtype=wp.vec3, device=device)
        self._scratch_b = wp.empty(capacity, dtype=wp.vec3, device=device)
        self._scratch_capacity = capacity

    def _upload_scratch(self, indices, a, b):
        k = indices.shape[0]
        self._ensure_scratch(k)
        self._wait_for_uploads()
        np.copyto(self._scratch_index_stage_np[:k], indices)
        np.copyto(self._scratch_a_stage_np[:k], a)
        wp.copy(self._scratch_index, self._scratch_index_stage, count=k)
        wp.copy(self._scratch_a, self._scratch_a_stage, count=k)
        if b is not None:
            np.copyto(self._scratch_b_stage_np[:k], b)
            wp.copy(self._scratch_b, self._scratch_b_stage, count=k)
        self._note_upload()

    def _download(self, host_pinned, device_array):
        """Device -> pinned host copy, then waits for it.

        ``wp.copy`` issues ``cudaMemcpyAsync`` on the device's stream and never
        synchronizes. For a PAGEABLE host destination CUDA makes that behave
        synchronously anyway, but a PINNED destination really is asynchronous,
        so reading the numpy view straight after the copy returns whatever the
        buffer held before (zeros, on the first call). Every readback therefore
        ends with a stream synchronize, which also waits for the fixed steps
        queued ahead of it.
        """
        wp.copy(host_pinned, device_array)
        if self._is_cuda:
            wp.synchronize_stream(self._device.stream)

    def _wait_for_uploads(self):
        """Pinned host-to-device copies are asynchronous: wait before reusing a staging buffer."""
        if self._upload_pending:
            wp.synchronize_event(self._upload_event)
            self._upload_pending = False

    def _note_upload(self):
        if self._upload_event is not None:
            self._device.stream.record_event(self._upload_event)
            self._upload_pending = True


# Above this many precomputed shape pairs the collision pipeline uses sweep-and-prune.
_EXPLICIT_BROAD_PHASE_MAX_PAIRS = 50_000
