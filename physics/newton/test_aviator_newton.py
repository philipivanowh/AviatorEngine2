"""Self-checking tests for aviator_newton, the Newton physics bridge.

Run from the repository root::

    .venv-newton/Scripts/python.exe physics/newton/test_aviator_newton.py

Plain asserts, no pytest. Every check is collected and printed as a summary
table at the end; the process exits non-zero if anything failed. The first run
on a machine with a cold Warp kernel cache compiles CUDA kernels and can take
several minutes - later runs load them from disk in a few seconds.

Options::

    --cpu-only     skip every CUDA test (useful on a machine without a GPU)
    --quick        skip the performance and solver-tuning sweeps
"""

from __future__ import annotations

import math
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import aviator_newton as av  # noqa: E402

import warp as wp  # noqa: E402

CPU_ONLY = "--cpu-only" in sys.argv
QUICK = "--quick" in sys.argv

# Enum numbers from the contract.
DYNAMIC, KINEMATIC = 0, 1
SPHERE, BOX, CAPSULE, PLANE = 0, 1, 2, 3

IDENTITY = (0.0, 0.0, 0.0, 1.0)
ZERO = (0.0, 0.0, 0.0)

_results: list[tuple[str, str, bool, str]] = []
_group = "?"
_timings: list[tuple[str, str]] = []


def group(name):
    global _group
    _group = name
    print(f"\n=== {name}")


def check(name, ok, detail=""):
    """Records one check. Returns ok, so callers can branch on it."""
    ok = bool(ok)
    _results.append((_group, name, ok, detail))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}{('  -  ' + detail) if detail else ''}")
    return ok


def close_to(name, value, expected, tol, unit=""):
    return check(name, abs(value - expected) <= tol, f"{value:.5f} vs {expected:.5f} +-{tol:g}{unit}")


def raises(name, exception, function, *args, **kwargs):
    try:
        function(*args, **kwargs)
    except exception as error:
        return check(name, True, f"{type(error).__name__}: {str(error)[:60]}")
    except Exception as error:  # noqa: BLE001
        return check(name, False, f"raised {type(error).__name__}, expected {exception.__name__}")
    return check(name, False, "no exception raised")


# --------------------------------------------------------------------------- helpers


def settings(device="cuda", graph=True, **overrides):
    base = {
        "gravity": (0.0, -9.81, 0.0),
        "fixed_dt": 1.0 / 60.0,
        "substeps": 10,
        "solver": "xpbd",
        "solver_iterations": 2,
        "device": device,
        "use_cuda_graph": graph,
    }
    base.update(overrides)
    return base


def world(device="cuda", graph=True, **overrides):
    return av.World(settings(device, graph, **overrides))


def ground(w, friction=0.8, restitution=0.0):
    """An infinite static plane at y = 0, normal +Y."""
    return w.add_collider(-1, PLANE, ZERO, IDENTITY, (1, 1, 1), 1.0, 1.0, friction, restitution)


def sphere_body(w, position, radius=0.5, mass=1.0, friction=0.5, restitution=0.0, linear=ZERO, angular=ZERO):
    b = w.add_body(DYNAMIC, position, IDENTITY, mass, linear, angular)
    w.add_collider(b, SPHERE, ZERO, IDENTITY, (1, 1, 1), radius, 0.0, friction, restitution)
    return b


def box_body(w, position, half=(0.5, 0.5, 0.5), mass=1.0, friction=0.8, restitution=0.0, linear=ZERO, angular=ZERO):
    b = w.add_body(DYNAMIC, position, IDENTITY, mass, linear, angular)
    w.add_collider(b, BOX, ZERO, IDENTITY, half, 0.5, 0.5, friction, restitution)
    return b


def poses(w, out=None):
    """Reads every pose and fails the run if anything is not finite (test 11)."""
    if out is None:
        out = np.empty((w.body_count(), 7), np.float32)
    w.read_body_poses(out)
    if not np.isfinite(out).all():
        raise AssertionError(f"non-finite pose read back:\n{out}")
    return out


def quat_angle(q):
    """Rotation angle in radians of a unit quaternion (qx, qy, qz, qw)."""
    return 2.0 * math.atan2(float(np.linalg.norm(q[:3])), abs(float(q[3])))


def run_steps(w, steps, chunk=1):
    for _ in range(0, steps, chunk):
        w.step(chunk)


# --------------------------------------------------------------------------- 1. sphere on a plane


def test_sphere_rest(device="cuda", graph=True):
    group("1. sphere dropped on a static plane")
    w = world(device, graph)
    radius = 0.5
    b = sphere_body(w, (0.0, 2.0, 0.0), radius=radius)
    ground(w)
    w.finalize()
    p = poses(w)
    close_to("starts where it was authored", float(p[b, 1]), 2.0, 1e-5, " m")
    run_steps(w, 180)  # 3 s
    p = poses(w)
    close_to("rests at y = radius", float(p[b, 1]), radius, 0.02, " m")
    check("did not drift sideways", abs(float(p[b, 0])) < 0.01 and abs(float(p[b, 2])) < 0.01,
          f"x={p[b, 0]:.5f} z={p[b, 2]:.5f}")
    w.close()


# --------------------------------------------------------------------------- 2. box rest and a stack


def test_box_rest_and_stack(device="cuda", graph=True):
    group("2. box at rest and a five-box stack")
    w = world(device, graph)
    half = 0.4
    b = box_body(w, (0.0, 2.0, 0.0), half=(half, half, half))
    ground(w)
    w.finalize()
    run_steps(w, 180)
    p = poses(w)
    close_to("single box rests at y = half height", float(p[b, 1]), half, 0.02, " m")
    w.close()

    # Five boxes, each resting on the one below, with a 1 mm gap so they do not
    # start interpenetrating.
    w = world(device, graph)
    half = 0.25
    gap = 0.001
    bodies = [box_body(w, (0.0, half + i * (2 * half + gap), 0.0), half=(half, half, half), friction=0.9)
              for i in range(5)]
    ground(w, friction=0.9)
    w.finalize()
    start = poses(w).copy()
    run_steps(w, 300)  # 5 s
    p = poses(w)
    top = bodies[-1]
    drift = math.hypot(float(p[top, 0]) - float(start[top, 0]), float(p[top, 2]) - float(start[top, 2]))
    heights = [float(p[b, 1]) for b in bodies]
    check("stack still standing after 5 s", heights[-1] > 4 * half,
          "heights " + ", ".join(f"{h:.3f}" for h in heights))
    check("top box barely moved sideways", drift < 0.05, f"drift {drift * 1000:.1f} mm")
    check("boxes stayed in order", all(heights[i] < heights[i + 1] for i in range(4)))
    w.close()


# --------------------------------------------------------------------------- 3. initial velocities


def test_initial_velocities(device="cuda", graph=True):
    group("3. initial linear and angular velocity are honored")
    w = world(device, graph, gravity=(0.0, 0.0, 0.0))
    vx = 3.0
    omega = 2.0  # rad/s about +Y
    linear = box_body(w, (0.0, 5.0, 0.0), linear=(vx, 0.0, 0.0))
    spinner = box_body(w, (0.0, 15.0, 0.0), angular=(0.0, omega, 0.0))
    ground(w)
    w.finalize()
    seconds = 0.5
    run_steps(w, int(round(seconds * 60)))
    p = poses(w)
    close_to("linear: x = vx * t", float(p[linear, 0]), vx * seconds, 0.02, " m")
    close_to("angular: rotation = omega * t", quat_angle(p[spinner, 3:7]), omega * seconds, 0.03, " rad")
    check("spin is about +Y only", abs(float(p[spinner, 3])) < 1e-3 and abs(float(p[spinner, 5])) < 1e-3,
          f"qx={p[spinner, 3]:.2e} qy={p[spinner, 4]:.4f} qz={p[spinner, 5]:.2e}")
    w.close()


# --------------------------------------------------------------------------- 4. restitution


def test_restitution(device="cuda", graph=True):
    group("4. restitution")
    peaks = {}
    for restitution in (0.0, 0.8):
        w = world(device, graph)
        radius = 0.3
        b = sphere_body(w, (0.0, 1.5, 0.0), radius=radius, restitution=restitution)
        ground(w, restitution=restitution)
        w.finalize()
        out = np.empty((w.body_count(), 7), np.float32)
        bounced = False
        peak = 0.0
        for _ in range(240):  # 4 s
            w.step(1)
            y = float(poses(w, out)[b, 1])
            if y < radius + 0.05:
                bounced = True
            if bounced:
                peak = max(peak, y)
        peaks[restitution] = peak
        w.close()
    check("restitution 0.0 does not bounce", peaks[0.0] < 0.4, f"peak y {peaks[0.0]:.4f} m")
    check("restitution 0.8 rebounds clearly higher", peaks[0.8] > peaks[0.0] + 0.3,
          f"peak y {peaks[0.8]:.4f} m vs {peaks[0.0]:.4f} m")


# --------------------------------------------------------------------------- 5. wrenches and impulses


def test_wrench_and_impulse(device="cuda", graph=True):
    group("5. wrenches and impulses")
    w = world(device, graph)
    mass = 2.0
    b = box_body(w, (0.0, 0.5, 0.0), mass=mass, friction=0.0)  # frictionless, so F = m a exactly
    ground(w, friction=0.0)
    w.finalize()

    # The wrench acts on every substep of the NEXT step() call, so the force
    # must be pushed with one step(n) call - that is the C++ call pattern too.
    force = 20.0
    seconds = 0.5
    ticks = int(round(seconds * 60))
    wrenches = np.zeros((w.body_count(), 6), np.float32)
    wrenches[b, 0] = force
    w.set_wrenches(wrenches)
    w.step(ticks)
    x1 = float(poses(w)[b, 0])
    expected = 0.5 * (force / mass) * seconds**2
    close_to("constant force: x = a t^2 / 2", x1, expected, 0.02, " m")

    # No new wrench: the box must coast at the velocity it reached, not accelerate.
    w.step(ticks)
    x2 = float(poses(w)[b, 0])
    coasting = (x2 - x1) / seconds
    close_to("wrench was cleared after step (coasts at v = a t)", coasting, (force / mass) * seconds, 0.05, " m/s")

    # And a wrench set once is not re-applied by a later step() call: pushing
    # with 30 separate step(1) calls only accelerates during the first.
    w.set_wrenches(wrenches)
    x3 = float(poses(w)[b, 0])
    run_steps(w, ticks)
    gained = (float(poses(w)[b, 0]) - x3) / seconds - coasting
    close_to("a wrench covers one step() call only", gained, (force / mass) * (1.0 / 60.0), 0.02, " m/s")

    impulse = 6.0
    z0 = float(poses(w)[b, 2])
    w.apply_impulses(np.array([b], np.int32), np.array([[0.0, 0.0, impulse]], np.float32))
    ticks = 6
    w.step(ticks)
    dz = float(poses(w)[b, 2]) - z0
    close_to("impulse: velocity jump = J / m", dz / (ticks / 60.0), impulse / mass, 0.02, " m/s")

    # A kinematic body ignores impulses.
    w.close()
    w = world(device, graph)
    k = w.add_body(KINEMATIC, (0.0, 1.0, 0.0), IDENTITY, 1.0, ZERO, ZERO)
    w.add_collider(k, BOX, ZERO, IDENTITY, (0.5, 0.5, 0.5), 0.5, 0.5, 0.8, 0.0)
    ground(w)
    w.finalize()
    w.apply_impulses(np.array([k], np.int32), np.array([[0.0, 0.0, 50.0]], np.float32))
    w.step(30)
    p = poses(w)
    check("kinematic body ignores impulses", abs(float(p[k, 2])) < 1e-4 and abs(float(p[k, 1]) - 1.0) < 1e-4,
          f"pose {p[k, :3]}")
    w.close()


# --------------------------------------------------------------------------- 6. kinematic bodies


def test_kinematic(device="cuda", graph=True):
    group("6. kinematic bodies reach targets and push dynamic bodies")
    w = world(device, graph)
    k = w.add_body(KINEMATIC, (-2.0, 0.5, 0.0), IDENTITY, 1.0, ZERO, ZERO)
    w.add_collider(k, BOX, ZERO, IDENTITY, (0.5, 0.5, 0.5), 0.5, 0.5, 0.5, 0.0)
    radius = 0.4
    s = sphere_body(w, (0.0, radius, 0.0), radius=radius)
    ground(w)
    w.finalize()

    run_steps(w, 30)  # let the sphere settle
    settled = poses(w)
    sphere_x0 = float(settled[s, 0])

    # Sweep the kinematic box from x = -2 to x = +1 over two seconds.
    indices = np.array([k], np.int32)
    target = np.zeros((1, 7), np.float32)
    steps = 120
    for i in range(steps):
        alpha = (i + 1) / steps
        target[0] = (-2.0 + 3.0 * alpha, 0.5, 0.0, 0.0, 0.0, 0.0, 1.0)
        w.set_kinematic_poses(indices, target)
        w.step(1)
    p = poses(w)
    close_to("kinematic body reached its final target x", float(p[k, 0]), 1.0, 1e-4, " m")
    close_to("kinematic body held its target y", float(p[k, 1]), 0.5, 1e-4, " m")
    check("kinematic body pushed the sphere out of the way", float(p[s, 0]) > sphere_x0 + 1.0,
          f"sphere x {sphere_x0:.3f} -> {p[s, 0]:.3f}")
    check("sphere stayed on the ground", abs(float(p[s, 1]) - radius) < 0.05, f"y {p[s, 1]:.4f}")

    # A rotation target, and then no target at all: the body must hold still.
    half_turn = (0.0, math.sin(math.pi / 4), 0.0, math.cos(math.pi / 4))  # 90 deg about Y
    target[0] = (1.0, 0.5, 0.0, *half_turn)
    w.set_kinematic_poses(indices, target)
    w.step(30)
    p = poses(w)
    close_to("kinematic rotation target reached", quat_angle(p[k, 3:7]), math.pi / 2, 1e-3, " rad")
    before = poses(w).copy()
    w.step(30)
    p = poses(w)
    check("kinematic body with no new target holds still",
          float(np.abs(p[k] - before[k]).max()) < 1e-5, f"max change {np.abs(p[k] - before[k]).max():.2e}")
    w.close()


# --------------------------------------------------------------------------- 7. write_body_poses


def test_write_body_poses(device="cuda", graph=True):
    group("7. write_body_poses teleports and zeroes velocities")
    w = world(device, graph)
    a = sphere_body(w, (0.0, 3.0, 0.0), radius=0.5)
    b = box_body(w, (2.0, 3.0, 0.0), half=(0.4, 0.4, 0.4))
    ground(w)
    w.finalize()
    authored = poses(w).copy()

    run_steps(w, 60)
    moved = poses(w)
    check("bodies moved before the restore", abs(float(moved[a, 1]) - 3.0) > 0.5, f"y {moved[a, 1]:.4f}")

    w.write_body_poses(authored)
    restored = poses(w)
    check("poses restored exactly", np.allclose(restored, authored, atol=1e-6),
          f"max error {np.abs(restored - authored).max():.2e}")

    # With velocities zeroed, one step of free fall must move by exactly the
    # distance a body starting from rest falls - not by more.
    w.step(1)
    after = poses(w)
    dt = 1.0 / 60.0
    fall = abs(float(after[a, 1]) - float(restored[a, 1]))
    check("velocity was zeroed (one step of fall from rest)", fall < 9.81 * dt * dt * 1.5 + 1e-4,
          f"fell {fall * 1000:.3f} mm, rest-fall bound {(9.81 * dt * dt * 1.5) * 1000:.3f} mm")

    # A pending wrench must be dropped by write_body_poses.
    wrenches = np.zeros((w.body_count(), 6), np.float32)
    wrenches[a, 0] = 500.0
    w.set_wrenches(wrenches)
    w.write_body_poses(authored)
    w.step(1)
    p = poses(w)
    check("pending wrench was dropped", abs(float(p[a, 0]) - float(authored[a, 0])) < 1e-4,
          f"x moved {abs(float(p[a, 0]) - float(authored[a, 0])):.2e} m")
    w.close()


# --------------------------------------------------------------------------- 8. compound body


def test_compound_body(device="cuda", graph=True):
    group("8. compound body: one box collider plus two offset spheres")
    w = world(device, graph)
    b = w.add_body(DYNAMIC, (0.0, 3.0, 0.0), IDENTITY, 6.0, ZERO, ZERO)
    w.add_collider(b, BOX, ZERO, IDENTITY, (0.5, 0.25, 0.25), 0.5, 0.5, 0.8, 0.0)
    w.add_collider(b, SPHERE, (0.8, 0.0, 0.0), IDENTITY, (1, 1, 1), 0.25, 0.0, 0.8, 0.0)
    w.add_collider(b, SPHERE, (-0.8, 0.0, 0.0), IDENTITY, (1, 1, 1), 0.25, 0.0, 0.8, 0.0)
    ground(w)
    w.finalize()
    run_steps(w, 240)  # 4 s
    p = poses(w)
    check("pose stayed finite", np.isfinite(p).all())
    check("did not tunnel through the plane", float(p[b, 1]) > 0.1, f"y {p[b, 1]:.4f} m")
    # The two spheres hang lowest, so the body settles with its origin at the
    # sphere radius (0.25), the same as the box half height here.
    close_to("landed at the expected height", float(p[b, 1]), 0.25, 0.03, " m")
    check("stayed roughly level", quat_angle(p[b, 3:7]) < 0.2, f"tilt {quat_angle(p[b, 3:7]):.4f} rad")
    w.close()


# --------------------------------------------------------------------------- 9. capsule along local Y


def test_capsule_axis(device="cuda", graph=True):
    group("9. capsule runs along local Y")
    w = world(device, graph)
    radius, half_height = 0.3, 0.7
    b = w.add_body(DYNAMIC, (0.0, 2.0, 0.0), IDENTITY, 1.0, ZERO, ZERO)
    w.add_collider(b, CAPSULE, ZERO, IDENTITY, (1, 1, 1), radius, half_height, 0.8, 0.0)
    ground(w)
    w.finalize()
    run_steps(w, 180)
    p = poses(w)
    close_to("upright capsule rests at half_height + radius", float(p[b, 1]), half_height + radius, 0.02, " m")
    check("stayed upright", quat_angle(p[b, 3:7]) < 0.1, f"tilt {quat_angle(p[b, 3:7]):.4f} rad")
    w.close()

    # Lying on its side, the same capsule must rest at just the radius.
    w = world(device, graph)
    on_side = (math.sin(math.pi / 4), 0.0, 0.0, math.cos(math.pi / 4))  # 90 deg about X: local Y -> world Z
    b = w.add_body(DYNAMIC, (0.0, 2.0, 0.0), on_side, 1.0, ZERO, ZERO)
    w.add_collider(b, CAPSULE, ZERO, IDENTITY, (1, 1, 1), radius, half_height, 0.8, 0.0)
    ground(w)
    w.finalize()
    run_steps(w, 180)
    p = poses(w)
    close_to("capsule on its side rests at radius", float(p[b, 1]), radius, 0.02, " m")
    w.close()


# --------------------------------------------------------------------------- 10. previous_poses_out


def test_previous_poses(device="cuda", graph=True):
    group("10. previous_poses_out semantics")
    w = world(device, graph)
    b = sphere_body(w, (0.0, 5.0, 0.0), radius=0.5)
    ground(w)
    w.finalize()
    authored = poses(w).copy()
    n = w.body_count()
    previous = np.empty((n, 7), np.float32)

    # num_steps == 1: the poses at the start of the call.
    before = poses(w).copy()
    w.step(1, previous)
    after = poses(w)
    check("num_steps=1: previous == pose at the start of the call",
          np.allclose(previous, before, atol=1e-6), f"max error {np.abs(previous - before).max():.2e}")
    check("num_steps=1: previous != pose after the call", not np.allclose(previous, after, atol=1e-5),
          f"y {previous[b, 1]:.5f} -> {after[b, 1]:.5f}")

    # num_steps == 3: the poses from before the final fixed step, i.e. after 2
    # steps. Checked by replaying the same two steps from the authored rest state.
    w.write_body_poses(authored)
    w.step(3, previous)
    w.write_body_poses(authored)
    w.step(2)
    two_steps = poses(w)
    check("num_steps=3: previous == pose after 2 steps",
          np.allclose(previous, two_steps, atol=1e-6), f"max error {np.abs(previous - two_steps).max():.2e}")

    # And it really is one step behind the end of the call.
    w.write_body_poses(authored)
    w.step(3, previous)
    end = poses(w)
    check("num_steps=3: previous is one step behind the end", not np.allclose(previous, end, atol=1e-5),
          f"y {previous[b, 1]:.5f} vs {end[b, 1]:.5f}")
    w.close()


# --------------------------------------------------------------------------- 11. validation


def test_validation(device="cuda", graph=True):
    group("11. input validation and lifecycle errors")
    w = world(device, graph)
    raises("step() before finalize()", RuntimeError, w.step, 1)
    raises("read_body_poses() before finalize()", RuntimeError, w.read_body_poses,
           np.empty((1, 7), np.float32))
    b = sphere_body(w, (0.0, 2.0, 0.0))
    raises("add_collider() with an out-of-range body", IndexError, w.add_collider,
           99, SPHERE, ZERO, IDENTITY, (1, 1, 1), 0.5, 0.0, 0.5, 0.0)
    raises("add_body() with a bad kind", ValueError, w.add_body, 7, ZERO, IDENTITY, 1.0, ZERO, ZERO)
    raises("add_body() with a zero quaternion", ValueError, w.add_body,
           DYNAMIC, ZERO, (0.0, 0.0, 0.0, 0.0), 1.0, ZERO, ZERO)
    raises("add_body() with non-positive mass", ValueError, w.add_body, DYNAMIC, ZERO, IDENTITY, 0.0, ZERO, ZERO)
    raises("add_collider() with a non-static plane", ValueError, w.add_collider,
           b, PLANE, ZERO, IDENTITY, (1, 1, 1), 1.0, 1.0, 0.5, 0.0)
    raises("add_collider() with restitution out of range", ValueError, w.add_collider,
           b, SPHERE, ZERO, IDENTITY, (1, 1, 1), 0.5, 0.0, 0.5, 1.5)
    ground(w)
    w.finalize()
    n = w.body_count()

    raises("add_body() after finalize()", RuntimeError, w.add_body, DYNAMIC, ZERO, IDENTITY, 1.0, ZERO, ZERO)
    raises("finalize() twice", RuntimeError, w.finalize)
    raises("read_body_poses() with the wrong shape", ValueError, w.read_body_poses, np.empty((n + 1, 7), np.float32))
    raises("read_body_poses() with the wrong dtype", TypeError, w.read_body_poses, np.empty((n, 7), np.float64))
    raises("read_body_poses() with a list", TypeError, w.read_body_poses, [[0.0] * 7] * n)
    raises("set_wrenches() with the wrong shape", ValueError, w.set_wrenches, np.zeros((n, 3), np.float32))
    raises("set_wrenches() with NaN", ValueError, w.set_wrenches, np.full((n, 6), np.nan, np.float32))
    raises("apply_impulses() with an out-of-range index", IndexError, w.apply_impulses,
           np.array([n + 5], np.int32), np.zeros((1, 3), np.float32))
    raises("apply_impulses() with int64 indices", TypeError, w.apply_impulses,
           np.array([0], np.int64), np.zeros((1, 3), np.float32))
    raises("set_kinematic_poses() on a dynamic body", ValueError, w.set_kinematic_poses,
           np.array([b], np.int32), np.zeros((1, 7), np.float32) + np.array([0, 0, 0, 0, 0, 0, 1], np.float32))
    raises("step() with a negative count", ValueError, w.step, -1)
    raises("step() with a float count", TypeError, w.step, 1.5)

    out = np.empty((n, 7), np.float32)
    w.step(0, out)  # a no-op that still reports the current poses
    check("step(0) is a no-op that fills previous_poses_out", np.isfinite(out).all(), f"y {out[b, 1]:.4f}")

    w.close()
    raises("step() after close()", RuntimeError, w.step, 1)
    raises("read_body_poses() after close()", RuntimeError, w.read_body_poses, out)
    check("body_count() still answers after close()", w.body_count() == n, f"{w.body_count()}")
    check("description() still answers after close()", "Newton" in w.description(), w.description())
    w.close()
    check("close() is idempotent", True)

    raises("unknown settings key", ValueError, av.World, {"gravity": ZERO, "nonsense": 1})
    raises("bad solver name", ValueError, av.World, settings(device, graph, solver="bogus"))
    raises("zero fixed_dt", ValueError, av.World, settings(device, graph, fixed_dt=0.0))
    raises("zero substeps", ValueError, av.World, settings(device, graph, substeps=0))
    bad_device = settings(device, graph)
    bad_device["device"] = "tpu"
    raises("bad device name", ValueError, av.World, bad_device)

    # A body with no collider cannot be given a mass distribution.
    w2 = world(device, graph)
    w2.add_body(DYNAMIC, (0.0, 1.0, 0.0), IDENTITY, 1.0, ZERO, ZERO)
    raises("finalize() with a collider-less body", ValueError, w2.finalize)
    w2.close()


# --------------------------------------------------------------------------- 12. performance


def test_performance(device="cuda", graph=True):
    group("12. performance: 60 mixed bodies")
    count = 60
    timings = {}
    for use_graph in ((True, False) if graph else (False,)):
        t0 = time.perf_counter()
        w = world(device, use_graph)
        for i in range(count):
            x = (i % 10) * 1.2 - 5.4
            z = (i // 10) * 1.2 - 3.0
            y = 0.5 + 0.05 * i
            if i % 2:
                sphere_body(w, (x, y, z), radius=0.35, mass=1.0)
            else:
                box_body(w, (x, y, z), half=(0.3, 0.3, 0.3), mass=1.0)
        ground(w)
        w.finalize()
        finalize_seconds = time.perf_counter() - t0
        active = w._use_graph  # noqa: SLF001 - reporting what actually happened

        run_steps(w, 30)  # settle and warm up
        wp.synchronize()
        reps = 120
        t0 = time.perf_counter()
        for _ in range(reps):
            w.step(1)
        wp.synchronize()
        per_step = (time.perf_counter() - t0) * 1000.0 / reps
        p = poses(w)
        check(f"{count} bodies simulate cleanly (graph={active})",
              np.isfinite(p).all() and float(p[:, 1].min()) > -0.2,
              f"lowest y {p[:, 1].min():.4f} m")
        timings[active] = per_step
        label = "graph ON " if active else "graph OFF"
        _timings.append((f"{count} bodies, 10 substeps, {label}", f"{per_step:.3f} ms / fixed step"))
        _timings.append((f"{count} bodies, finalize ({label.strip()})", f"{finalize_seconds:.2f} s"))
        print(f"    {label}: {per_step:.3f} ms per fixed step, finalize {finalize_seconds:.2f} s")
        w.close()
    if True in timings and False in timings:
        speedup = timings[False] / timings[True]
        check("the CUDA graph is not slower than replaying launches", speedup > 0.95, f"{speedup:.2f}x speedup")


# --------------------------------------------------------------------------- 13. solver tuning sweep


def test_tuning(device="cuda", graph=True):
    group("13. substeps / solver_iterations sweep for stable stacking at 60 Hz")
    half, gap, seconds = 0.25, 0.001, 3.0
    rows = []
    for substeps, iterations in ((4, 2), (8, 2), (10, 2), (10, 4), (16, 2), (20, 2)):
        w = world(device, graph, substeps=substeps, solver_iterations=iterations)
        bodies = [box_body(w, (0.0, half + i * (2 * half + gap), 0.0), half=(half, half, half), friction=0.9)
                  for i in range(5)]
        ground(w, friction=0.9)
        w.finalize()
        start = poses(w).copy()
        run_steps(w, int(seconds * 60))
        p = poses(w)
        top = bodies[-1]
        drift = math.hypot(float(p[top, 0]) - float(start[top, 0]), float(p[top, 2]) - float(start[top, 2]))
        sink = float(start[bodies[0], 1]) - float(p[bodies[0], 1])
        standing = float(p[top, 1]) > 4 * half
        wp.synchronize()
        t0 = time.perf_counter()
        for _ in range(60):
            w.step(1)
        wp.synchronize()
        per_step = (time.perf_counter() - t0) * 1000.0 / 60
        rows.append((substeps, iterations, drift * 1000, sink * 1000, standing, per_step))
        print(f"    substeps={substeps:2d} iterations={iterations}: top drift {drift * 1000:6.2f} mm, "
              f"bottom sink {sink * 1000:5.2f} mm, standing={standing}, {per_step:.3f} ms/step")
        w.close()
    stable = [r for r in rows if r[4] and r[2] < 50.0]
    check("at least one setting stacks stably", bool(stable), f"{len(stable)} of {len(rows)} settings stable")
    for substeps, iterations, drift, _sink, standing, per_step in rows:
        _timings.append((f"stack 5 boxes, substeps={substeps} iterations={iterations}",
                         f"{'stable' if standing and drift < 50 else 'UNSTABLE'}, "
                         f"top drift {drift:.2f} mm, {per_step:.3f} ms/step"))


# --------------------------------------------------------------------------- 14. CPU device


def test_cpu_smoke():
    group("14. CPU device smoke run")
    w = world("cpu", False)
    check("cpu world reports the cpu device", "cpu" in w.description(), w.description())
    radius = 0.5
    b = sphere_body(w, (0.0, 1.5, 0.0), radius=radius)
    k = w.add_body(KINEMATIC, (2.0, 0.5, 0.0), IDENTITY, 1.0, ZERO, ZERO)
    w.add_collider(k, BOX, ZERO, IDENTITY, (0.5, 0.5, 0.5), 0.5, 0.5, 0.5, 0.0)
    ground(w)
    w.finalize()
    check("no CUDA graph on the cpu", "CUDA graph" not in w.description(), w.description())
    previous = np.empty((w.body_count(), 7), np.float32)
    run_steps(w, 120, chunk=2)
    w.set_kinematic_poses(np.array([k], np.int32),
                          np.array([[2.0, 1.5, 0.0, 0.0, 0.0, 0.0, 1.0]], np.float32))
    w.step(30, previous)
    p = poses(w)
    close_to("cpu: sphere rests at y = radius", float(p[b, 1]), radius, 0.02, " m")
    close_to("cpu: kinematic target reached", float(p[k, 1]), 1.5, 1e-4, " m")
    check("cpu: previous poses are finite", np.isfinite(previous).all())
    w.close()


# --------------------------------------------------------------------------- extras


def test_mass_by_volume(device="cuda", graph=True):
    group("15. mass spread over colliders by volume")
    # Two identical bodies, one with a single box collider and one with the same
    # box split into two half-size boxes: they must fall and land identically.
    w = world(device, graph)
    single = w.add_body(DYNAMIC, (0.0, 2.0, 0.0), IDENTITY, 4.0, ZERO, ZERO)
    w.add_collider(single, BOX, ZERO, IDENTITY, (0.5, 0.25, 0.25), 0.5, 0.5, 0.8, 0.0)
    split = w.add_body(DYNAMIC, (3.0, 2.0, 0.0), IDENTITY, 4.0, ZERO, ZERO)
    w.add_collider(split, BOX, (-0.25, 0.0, 0.0), IDENTITY, (0.25, 0.25, 0.25), 0.5, 0.5, 0.8, 0.0)
    w.add_collider(split, BOX, (0.25, 0.0, 0.0), IDENTITY, (0.25, 0.25, 0.25), 0.5, 0.5, 0.8, 0.0)
    ground(w)
    w.finalize()
    inv_mass = w._model.body_inv_mass.numpy()  # noqa: SLF001 - checking the derived mass
    close_to("single-collider body has the requested mass", 1.0 / float(inv_mass[single]), 4.0, 1e-3, " kg")
    close_to("two-collider body has the requested mass", 1.0 / float(inv_mass[split]), 4.0, 1e-3, " kg")
    run_steps(w, 180)
    p = poses(w)
    close_to("both land at the same height", float(p[single, 1]), float(p[split, 1]), 0.02, " m")
    w.close()


def test_set_body_velocities(device="cuda", graph=True):
    group("16. set_body_velocities")
    w = world(device, graph, gravity=ZERO)
    a = sphere_body(w, (0.0, 5.0, 0.0))
    b = sphere_body(w, (5.0, 5.0, 0.0))
    ground(w)
    w.finalize()
    indices = np.array([a, b], np.int32)
    linear = np.array([[2.0, 0.0, 0.0], [0.0, 0.0, -3.0]], np.float32)
    angular = np.array([[0.0, 1.0, 0.0], [0.0, 0.0, 0.0]], np.float32)
    w.set_body_velocities(indices, linear, angular)
    seconds = 0.5
    run_steps(w, int(seconds * 60))
    p = poses(w)
    close_to("body a moved along +x", float(p[a, 0]), 2.0 * seconds, 0.02, " m")
    close_to("body b moved along -z", float(p[b, 2]), -3.0 * seconds, 0.02, " m")
    close_to("body a spun about +y", quat_angle(p[a, 3:7]), 1.0 * seconds, 0.03, " rad")
    w.close()


def test_no_bodies(device="cuda", graph=True):
    group("17. degenerate scenes")
    w = world(device, graph)
    ground(w)
    w.finalize()
    check("a world with only static geometry finalizes", w.body_count() == 0)
    empty = np.empty((0, 7), np.float32)
    w.read_body_poses(empty)
    w.step(3, empty)
    check("stepping a body-less world is a no-op", True)
    w.close()


# --------------------------------------------------------------------------- main


def main():
    started = time.perf_counter()
    print(f"aviator_newton bridge version {av.BRIDGE_VERSION}")
    print(f"numpy {np.__version__}, warp {wp.__version__}")

    cuda = wp.is_cuda_available() and not CPU_ONLY
    print(f"CUDA available: {wp.is_cuda_available()}  (running CUDA tests: {cuda})")

    if cuda:
        device, graph = "cuda", True
        for test in (
            test_sphere_rest,
            test_box_rest_and_stack,
            test_initial_velocities,
            test_restitution,
            test_wrench_and_impulse,
            test_kinematic,
            test_write_body_poses,
            test_compound_body,
            test_capsule_axis,
            test_previous_poses,
            test_validation,
            test_mass_by_volume,
            test_set_body_velocities,
            test_no_bodies,
        ):
            test(device, graph)
        # The same physics must come out with the graph disabled.
        group("18. same results without the CUDA graph")
        w = world("cuda", False)
        b = sphere_body(w, (0.0, 2.0, 0.0), radius=0.5)
        ground(w)
        w.finalize()
        check("cuda without a graph reports no graph", "CUDA graph" not in w.description(), w.description())
        run_steps(w, 180)
        close_to("sphere still rests at y = radius", float(poses(w)[b, 1]), 0.5, 0.02, " m")
        w.close()
        if not QUICK:
            test_performance(device, graph)
            test_tuning(device, graph)
    else:
        for test in (
            test_sphere_rest,
            test_box_rest_and_stack,
            test_initial_velocities,
            test_wrench_and_impulse,
            test_kinematic,
            test_previous_poses,
            test_validation,
        ):
            test("cpu", False)

    test_cpu_smoke()

    # ---- summary
    width = max(len(name) for _group, name, _ok, _detail in _results) + 2
    print("\n" + "=" * (width + 58))
    print("SUMMARY")
    print("=" * (width + 58))
    current = None
    for group_name, name, ok, detail in _results:
        if group_name != current:
            current = group_name
            print(f"\n{group_name}")
        print(f"  {'PASS' if ok else 'FAIL'}  {name:<{width}} {detail}")

    if _timings:
        print("\n" + "=" * (width + 58))
        print("TIMINGS")
        print("=" * (width + 58))
        for label, value in _timings:
            print(f"  {label:<52} {value}")

    failed = [(g, n, d) for g, n, ok, d in _results if not ok]
    total = len(_results)
    print("\n" + "=" * (width + 58))
    print(f"{total - len(failed)} of {total} checks passed in {time.perf_counter() - started:.1f} s")
    if failed:
        print("\nFAILED:")
        for group_name, name, detail in failed:
            print(f"  {group_name} / {name}: {detail}")
        return 1
    print("ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
