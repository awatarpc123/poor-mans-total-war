"""
Poor Man's Total War - British Line Infantry Generator (High-Detail v3)
Run in Blender (Scripting tab). Press Alt+P or click "Run Script".

Z=0 at ground, model faces +Y.  Export: FBX, Z Up, -Y Forward, Scale 1.0.

ARCHITECTURE — no puppet look:
  * Arms:  ONE ring_prism_mesh shoulder→wrist.  Shoulder bulge, elbow bump,
           and forearm taper are baked into the ring radius sequence.
           No ShoulderJoint / ElbowJoint objects exist.
  * Legs:  Thigh (hip→knee) and shin (knee→ankle) are separate meshes but
           their shared ring at KNEE_Z has IDENTICAL radii, so the
           subdivided surfaces are visually flush — no ball visible.
  * Torso: 9 elliptical sections (wide shoulder → cinched waist → hip).
  * Head:  ring_prism_mesh, 9 anatomical sections (chin → crown).
  * Coat tails: flat fabric panels (11 mm thick), not pillars.
  * Cross-belts: narrowed (38 mm wide, 8 mm thick), projected onto the
                 torso ellipse so they hug the chest.
  * Organic parts: Subdivision Surface level 2 + Shade Smooth (applied).
  * Flat panels (tails, lapels, belts): Subdivision level 1.
  * No polycount limit.
"""

import bpy
import bmesh
from mathutils import Vector
import math


# ===========================================================================
# Helpers
# ===========================================================================

def clear_scene():
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    for col in list(bpy.data.collections):
        bpy.data.collections.remove(col)
    for block in (bpy.data.meshes, bpy.data.materials):
        for item in list(block):
            if item.users == 0:
                block.remove(item)


def make_material(name, r, g, b, rough=0.8, metal=0.0):
    m = bpy.data.materials.new(name=name)
    m.use_nodes = True
    b_ = m.node_tree.nodes["Principled BSDF"]
    b_.inputs["Base Color"].default_value = (r, g, b, 1.0)
    b_.inputs["Roughness"].default_value = rough
    b_.inputs["Metallic"].default_value = metal
    return m


def select_only(obj):
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


def bm_to_object(bm, name, location=(0, 0, 0)):
    mesh = bpy.data.meshes.new(name + "_mesh")
    bm.to_mesh(mesh)
    bm.free()
    obj = bpy.data.objects.new(name, mesh)
    obj.location = location
    bpy.context.scene.collection.objects.link(obj)
    return obj


def finalize_organic(obj, levels=2):
    """Shade smooth + apply Subdivision Surface."""
    select_only(obj)
    bpy.ops.object.shade_smooth()
    mod = obj.modifiers.new(name="Subsurf", type='SUBSURF')
    mod.levels = levels
    mod.render_levels = levels
    bpy.ops.object.modifier_apply(modifier=mod.name)
    return obj


def join_objects(objects, name):
    bpy.ops.object.select_all(action='DESELECT')
    for o in objects:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]
    bpy.ops.object.join()
    bpy.context.active_object.name = name
    return bpy.context.active_object


# ---------------------------------------------------------------------------
# Mesh primitives
# ---------------------------------------------------------------------------

def ring_prism_mesh(rings):
    """
    Closed prism from a list of cross-section rings.
    Each ring: list of (x, y, z).  All rings must have the same vertex count.
    First ring  → bottom cap.  Last ring → top cap.
    """
    bm = bmesh.new()
    n = len(rings[0])
    vr = [[bm.verts.new(Vector(p)) for p in ring] for ring in rings]
    bm.verts.ensure_lookup_table()
    for ri in range(len(vr) - 1):
        a, b = vr[ri], vr[ri + 1]
        for i in range(n):
            j = (i + 1) % n
            bm.faces.new([a[i], a[j], b[j], b[i]])
    bm.faces.new(vr[0])
    bm.faces.new(list(reversed(vr[-1])))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    return bm


def ellipse_ring(cx, cy, z, rx, ry, n=14):
    """Elliptical ring at world-Z z, centred at (cx, cy)."""
    return [(cx + rx * math.cos(2 * math.pi * k / n),
             cy + ry * math.sin(2 * math.pi * k / n),
             z)
            for k in range(n)]


def tapered_cone_mesh(r_base, r_top, height, segments=16):
    """r_base at local z = -height/2,  r_top at z = +height/2."""
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False,
                          segments=segments,
                          radius1=r_base, radius2=r_top, depth=height)
    return bm


def sphere_mesh(radius, u=16, v=10, scale=(1.0, 1.0, 1.0)):
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=u, v_segments=v, radius=radius)
    if scale != (1.0, 1.0, 1.0):
        bmesh.ops.scale(bm, vec=Vector(scale), verts=bm.verts)
    return bm


# ===========================================================================
# Materials  (Napoleonic British line infantry palette)
# ===========================================================================
MAT = {}


def build_materials():
    MAT["coat"]     = make_material("BritInf_Coat",      0.58, 0.05, 0.05)
    MAT["facing"]   = make_material("BritInf_Facing",    0.90, 0.88, 0.82)
    MAT["trousers"] = make_material("BritInf_Trousers",  0.62, 0.62, 0.60)
    MAT["gaiter"]   = make_material("BritInf_Gaiter",    0.07, 0.07, 0.07)
    MAT["shako"]    = make_material("BritInf_Shako",     0.05, 0.05, 0.05)
    MAT["skin"]     = make_material("BritInf_Skin",      0.80, 0.61, 0.47)
    MAT["brass"]    = make_material("BritInf_Brass",     0.74, 0.56, 0.11,
                                    rough=0.35, metal=0.9)
    MAT["belt"]     = make_material("BritInf_Crossbelt", 0.93, 0.91, 0.86)
    MAT["boot"]     = make_material("BritInf_Boot",      0.06, 0.06, 0.06)
    MAT["wood"]     = make_material("Musket_Wood",       0.34, 0.19, 0.09)
    MAT["steel"]    = make_material("Musket_Steel",      0.30, 0.31, 0.33,
                                    rough=0.3, metal=0.9)


def mat(obj, key):
    obj.data.materials.append(MAT[key])
    return obj


# ===========================================================================
# Skeleton dimensions  (metres, Z = 0 at ground)
# ===========================================================================
# Target body height ≈ 1.75 m (without shako).  Arms per requirements.

FOOT_SZ     = 0.060
LOWER_LEG_H = 0.36
UPPER_LEG_H = 0.38
TORSO_H     = 0.48
NECK_H      = 0.085
HEAD_R      = 0.115

ANKLE_Z    = FOOT_SZ                            # 0.060
KNEE_Z     = ANKLE_Z    + LOWER_LEG_H           # 0.420
HIP_Z      = KNEE_Z     + UPPER_LEG_H           # 0.800
TORSO_Z    = HIP_Z      + TORSO_H / 2           # 1.040  torso centre
SHOULDER_Z = HIP_Z      + TORSO_H               # 1.280
NECK_Z     = SHOULDER_Z + NECK_H / 2            # 1.3225
HEAD_Z     = SHOULDER_Z + NECK_H + HEAD_R * 0.85  # ≈ 1.463

HIP_OFFSET_X = 0.095
UPPER_ARM_H  = 0.28
LOWER_ARM_H  = 0.24

# Torso profile table: (z_world, half_width_x, half_depth_y)
# Used for surface-projection by belts, lapels and coat tails.
TORSO_CROSS = [
    (SHOULDER_Z,          0.230, 0.150),   # shoulder top
    (SHOULDER_Z - 0.06,   0.220, 0.144),   # upper chest
    (SHOULDER_Z - 0.12,   0.208, 0.138),   # chest
    (SHOULDER_Z - 0.20,   0.190, 0.128),   # lower chest
    (TORSO_Z    + 0.06,   0.174, 0.118),   # above waist
    (TORSO_Z,             0.168, 0.114),   # waist (narrowest)
    (TORSO_Z    - 0.06,   0.172, 0.116),   # below waist
    (HIP_Z      + 0.10,   0.180, 0.120),   # upper hip
    (HIP_Z,               0.178, 0.118),   # hip bottom
]


def torso_profile(z):
    """Return (half_width, half_depth) of the torso ellipse at world Z."""
    pts = TORSO_CROSS
    if z >= pts[0][0]:  return pts[0][1], pts[0][2]
    if z <= pts[-1][0]: return pts[-1][1], pts[-1][2]
    for i in range(len(pts) - 1):
        z0, w0, d0 = pts[i]
        z1, w1, d1 = pts[i + 1]
        if z1 <= z <= z0:
            t = (z - z0) / (z1 - z0)
            return w0 + (w1 - w0) * t, d0 + (d1 - d0) * t
    return pts[-1][1], pts[-1][2]


# ===========================================================================
# TORSO  —  9 elliptical sections
# ===========================================================================

def make_torso():
    N = 16
    rings = [ellipse_ring(0, 0, z, hw, hd, n=N) for z, hw, hd in TORSO_CROSS]
    bm = ring_prism_mesh(rings)
    obj = bm_to_object(bm, "Torso", (0, 0, 0))
    mat(obj, "coat")
    finalize_organic(obj, levels=2)
    return obj


# ===========================================================================
# COAT TAILS  —  thin flat fabric panels, not pillars
# ===========================================================================

def make_coat_tails():
    """
    Two rear tails running from the waist hem down to just above the knee.
    Cross-section per ring is a flat rectangle (11 mm thick) whose back face
    tracks the torso's back surface via torso_profile().
    """
    parts = []
    top_z   = HIP_Z + 0.08
    bot_z   = KNEE_Z + 0.14
    n_steps = 6

    for sign in (+1, -1):
        rings = []
        for s in range(n_steps + 1):
            t      = s / n_steps
            z      = top_z + (bot_z - top_z) * t
            x_in   = sign * (0.015 + 0.008 * t)    # inner edge, slowly widens
            x_out  = sign * (0.130 + 0.055 * t)    # outer edge flares at hem
            _, hd  = torso_profile(z)
            y_back  = -(hd + 0.004)                # just outside the torso back
            y_front = y_back - 0.011               # 11 mm thick — fabric
            rings.append([
                (x_in,  y_front, z),
                (x_out, y_front, z),
                (x_out, y_back,  z),
                (x_in,  y_back,  z),
            ])
        bm = ring_prism_mesh(rings)
        obj = bm_to_object(bm, f"CoatTail_{'L' if sign > 0 else 'R'}", (0, 0, 0))
        mat(obj, "coat")
        finalize_organic(obj, levels=1)
        parts.append(obj)

    return parts


# ===========================================================================
# LAPELS + COLLAR
# ===========================================================================

def make_lapels():
    """
    Two facing-colour plastron strips on the chest front.
    Each strip is projected off the torso ellipse surface (+5 mm stand-off).
    """
    parts = []
    z_top   = SHOULDER_Z - 0.04
    z_bot   = TORSO_Z    + 0.04
    n_steps = 4

    for sign in (+1, -1):
        rings = []
        for s in range(n_steps + 1):
            t     = s / n_steps
            z     = z_top + (z_bot - z_top) * t
            _, hd = torso_profile(z)
            x_in  = sign * (0.014 + 0.005 * (1 - t))
            x_out = sign * (0.076 - 0.012 * t)
            y_s   = hd + 0.005
            th    = 0.010
            rings.append([
                (x_in,  y_s + th, z),
                (x_out, y_s + th, z),
                (x_out, y_s,      z),
                (x_in,  y_s,      z),
            ])
        bm = ring_prism_mesh(rings)
        obj = bm_to_object(bm, f"Lapel_{'L' if sign > 0 else 'R'}", (0, 0, 0))
        mat(obj, "facing")
        finalize_organic(obj, levels=1)
        parts.append(obj)

    # Brass button rows
    for sign in (+1, -1):
        for s in range(6):
            t     = s / 5
            z     = z_top + (z_bot - z_top) * t
            _, hd = torso_profile(z)
            bm    = sphere_mesh(0.010, u=10, v=8)
            btn   = bm_to_object(bm, "Button", (sign * 0.017, hd + 0.017, z))
            mat(btn, "brass")
            select_only(btn)
            bpy.ops.object.shade_smooth()
            parts.append(btn)

    return parts


def make_collar():
    """Standing facing-colour collar at the neck base."""
    bm  = tapered_cone_mesh(r_base=0.080, r_top=0.068, height=0.062, segments=16)
    obj = bm_to_object(bm, "Collar", (0, 0.010, SHOULDER_Z + 0.020))
    mat(obj, "facing")
    finalize_organic(obj, levels=1)
    return obj


# ===========================================================================
# HEAD  —  ring_prism_mesh with 9 anatomical sections (chin → crown)
# ===========================================================================

def make_head():
    """
    Each ring captures a key skull level: chin tip, jaw, cheekbones, temples
    (widest), parietal, crown apex.  cy_offset shifts each ring's centre
    forward in +Y to give a face projection (the face is not a perfect cylinder).
    Subdivision Level 2 fuses everything into a smooth, recognisably human head.
    """
    N       = 16
    BOT     = HEAD_Z - HEAD_R * 1.05   # chin level
    TOP     = HEAD_Z + HEAD_R * 0.95   # crown level

    # (z, rx, ry, cy_offset)
    sects = [
        (BOT,                      0.036, 0.030, -0.010),  # chin tip
        (BOT + HEAD_R * 0.22,      0.080, 0.068, -0.005),  # jaw
        (BOT + HEAD_R * 0.50,      0.102, 0.088,  0.000),  # cheekbones
        (BOT + HEAD_R * 0.72,      0.108, 0.095,  0.005),  # upper cheek
        (BOT + HEAD_R * 0.95,      0.110, 0.097,  0.004),  # temples — widest
        (BOT + HEAD_R * 1.30,      0.106, 0.094,  0.000),  # parietal
        (BOT + HEAD_R * 1.60,      0.094, 0.088, -0.004),  # upper parietal
        (TOP - HEAD_R * 0.15,      0.066, 0.062, -0.006),  # crown
        (TOP,                      0.030, 0.028, -0.006),  # apex
    ]
    rings = [ellipse_ring(0, cy, z, rx, ry, n=N) for z, rx, ry, cy in sects]
    bm  = ring_prism_mesh(rings)
    obj = bm_to_object(bm, "Head", (0, 0, 0))
    mat(obj, "skin")
    finalize_organic(obj, levels=2)
    return obj


def make_neck():
    bm  = tapered_cone_mesh(r_base=0.054, r_top=0.042, height=NECK_H + 0.025,
                             segments=14)
    obj = bm_to_object(bm, "Neck", (0, 0, NECK_Z))
    mat(obj, "skin")
    finalize_organic(obj, levels=2)
    return obj


# ===========================================================================
# ARMS  —  ONE ring_prism_mesh per arm, shoulder cap → wrist
# ===========================================================================

def make_arm(side):
    """
    The full arm is a single continuous ring_prism_mesh. Radii at each
    section define:
      shoulder cap (wide) → deltoid → upper arm taper → lower upper arm →
      elbow bulge (olecranon) → forearm → forearm taper → wrist.

    No ShoulderJoint or ElbowJoint objects.  Subdivision Level 2 makes the
    surface flow organically through the joint areas.
    """
    parts = []
    sign  = 1 if side == 'L' else -1
    # arm centre: slightly inside the shoulder edge → looks embedded, not floating
    arm_x = sign * (TORSO_CROSS[0][1] - 0.008)   # ≈ ±0.222

    shoulder_top = SHOULDER_Z - 0.010
    elbow_z      = shoulder_top - UPPER_ARM_H
    wrist_z      = elbow_z - LOWER_ARM_H
    N = 14

    # sections from shoulder (high Z) down to wrist (low Z)
    # (z,                    rx,    ry)
    sects = [
        (shoulder_top,          0.072, 0.066),  # shoulder cap
        (shoulder_top - 0.040,  0.070, 0.064),  # deltoid
        (shoulder_top - 0.100,  0.062, 0.057),  # upper arm
        (shoulder_top - 0.180,  0.056, 0.052),  # mid upper arm
        (elbow_z      + 0.050,  0.052, 0.048),  # lower upper arm
        (elbow_z,               0.060, 0.054),  # elbow / olecranon bulge
        (elbow_z      - 0.020,  0.052, 0.047),  # just below elbow
        (elbow_z      - 0.080,  0.050, 0.046),  # upper forearm
        (elbow_z      - 0.150,  0.046, 0.043),  # mid forearm
        (elbow_z      - 0.200,  0.042, 0.039),  # lower forearm
        (wrist_z      + 0.025,  0.038, 0.035),  # pre-wrist
        (wrist_z,               0.034, 0.032),  # wrist
    ]

    rings = [ellipse_ring(arm_x, 0, z, rx, ry, n=N) for z, rx, ry in sects]
    bm    = ring_prism_mesh(rings)
    arm_obj = bm_to_object(bm, f"Arm_{side}", (0, 0, 0))
    mat(arm_obj, "coat")
    finalize_organic(arm_obj, levels=2)
    parts.append(arm_obj)

    # Cuff — facing colour, separate object at the wrist
    bm2  = tapered_cone_mesh(r_base=0.048, r_top=0.042, height=0.055, segments=16)
    cuff = bm_to_object(bm2, f"Cuff_{side}", (arm_x, 0, wrist_z + 0.022))
    mat(cuff, "facing")
    finalize_organic(cuff, levels=2)
    parts.append(cuff)

    # Epaulette — flattened blob sitting on the shoulder cap
    bm3 = sphere_mesh(0.052, u=14, v=10, scale=(1.40, 0.68, 0.48))
    ep  = bm_to_object(bm3, f"Epaulette_{side}", (arm_x, 0, shoulder_top + 0.048))
    mat(ep, "facing")
    finalize_organic(ep, levels=2)
    parts.append(ep)

    # Hand — fist shape from a flattened sphere
    bm4  = sphere_mesh(0.040, u=14, v=10, scale=(0.82, 0.68, 1.20))
    hand = bm_to_object(bm4, f"Hand_{side}", (arm_x, 0, wrist_z - 0.045))
    mat(hand, "skin")
    finalize_organic(hand, levels=2)
    parts.append(hand)

    return parts


# ===========================================================================
# LEGS  —  thigh + shin with a SHARED knee ring (no ball joint)
# ===========================================================================

def make_leg(side):
    """
    Thigh and shin are separate meshes for material purposes (trousers /
    gaiter), but their rings at KNEE_Z are IDENTICAL in (x,y), so the two
    subdivided surfaces meet flush — no visible ball or gap at the knee.
    """
    parts = []
    sign  = 1 if side == 'L' else -1
    hip_x = sign * HIP_OFFSET_X
    N     = 14

    # ---- THIGH  (hip → knee, trousers) ----
    # Top ring is wide to partially embed into the torso bottom.
    # Bottom ring at KNEE_Z is the SHARED ring — must match shin top.
    thigh_sects = [
        (HIP_Z,             0.095, 0.090),  # hip top — overlaps torso hip
        (HIP_Z - 0.040,     0.088, 0.083),  # upper thigh
        (HIP_Z - 0.120,     0.078, 0.074),  # mid thigh
        (HIP_Z - 0.240,     0.066, 0.063),  # lower thigh
        (KNEE_Z + 0.060,    0.063, 0.060),  # just above knee
        (KNEE_Z,            0.070, 0.066),  # SHARED knee ring ← matches shin[0]
    ]
    rings  = [ellipse_ring(hip_x, 0, z, rx, ry, n=N) for z, rx, ry in thigh_sects]
    bm     = ring_prism_mesh(rings)
    thigh  = bm_to_object(bm, f"Thigh_{side}", (0, 0, 0))
    mat(thigh, "trousers")
    finalize_organic(thigh, levels=2)
    parts.append(thigh)

    # ---- SHIN / GAITER  (knee → ankle, black gaiter) ----
    # sects[0] must match thigh sects[-1] exactly: (KNEE_Z, 0.070, 0.066)
    shin_sects = [
        (KNEE_Z,            0.070, 0.066),  # SHARED knee ring ← matches thigh[-1]
        (KNEE_Z - 0.040,    0.067, 0.063),  # just below knee
        (KNEE_Z - 0.100,    0.072, 0.068),  # calf belly (gastrocnemius)
        (KNEE_Z - 0.200,    0.064, 0.060),  # mid calf
        (KNEE_Z - 0.280,    0.050, 0.048),  # tapering shin
        (ANKLE_Z + 0.040,   0.042, 0.040),  # lower shin
        (ANKLE_Z,           0.038, 0.036),  # ankle
    ]
    rings  = [ellipse_ring(hip_x, 0, z, rx, ry, n=N) for z, rx, ry in shin_sects]
    bm     = ring_prism_mesh(rings)
    shin   = bm_to_object(bm, f"Shin_{side}", (0, 0, 0))
    mat(shin, "gaiter")
    finalize_organic(shin, levels=2)
    parts.append(shin)

    parts.append(make_boot(side))
    return parts


def make_boot(side):
    """
    7-sided boot prism: two heel verts, two ball verts, rounded toe.
    Object origin at ankle level so it sits cleanly on the ground.
    """
    sign  = 1 if side == 'L' else -1
    hip_x = sign * HIP_OFFSET_X

    hw, fw          = 0.040, 0.046
    heel_y, ball_y  = -0.034, 0.022
    toe_y           = 0.125
    top_z, sole_z   = FOOT_SZ / 2, -FOOT_SZ / 2

    cross = [
        (-hw,        heel_y),
        ( hw,        heel_y),
        ( fw,        ball_y),
        ( fw * 0.60, toe_y * 0.88),
        ( 0,         toe_y),
        (-fw * 0.60, toe_y * 0.88),
        (-fw,        ball_y),
    ]
    top = [(x, y, top_z)  for x, y in cross]
    bot = [(x, y, sole_z) for x, y in cross]

    bm  = ring_prism_mesh([bot, top])
    obj = bm_to_object(bm, f"Boot_{side}", (hip_x, 0.0, ANKLE_Z - FOOT_SZ / 2))
    mat(obj, "boot")
    finalize_organic(obj, levels=2)
    return obj


# ===========================================================================
# CROSS-BELTS  —  narrow straps projected onto the torso ellipse
# ===========================================================================

def make_crossbelts():
    """
    Each belt is parametrised as a diagonal line in the X-Z plane; at every
    sample the belt edge X coordinates are projected onto the front surface of
    the torso ellipse so the strap hugs the chest.
    width=38 mm, thick=8 mm (thinner than previous version).
    """
    parts = []

    def belt(name, x0, z0, x1, z1, width=0.038, thick=0.008):
        steps   = 20
        centres = [(x0 + (x1 - x0) * s / steps,
                    z0 + (z1 - z0) * s / steps)
                   for s in range(steps + 1)]
        rings = []
        for i, (x, z) in enumerate(centres):
            if i < len(centres) - 1:
                dx, dz = centres[i + 1][0] - x, centres[i + 1][1] - z
            else:
                dx, dz = x - centres[i - 1][0], z - centres[i - 1][1]
            L  = math.hypot(dx, dz) or 1e-6
            px, pz = -dz / L, dx / L            # perpendicular in X-Z
            hw_b = width / 2
            ring = []
            for ws in (+1, -1):
                ex = x  + px * hw_b * ws
                ez = z  + pz * hw_b * ws
                hw_t, hd_t = torso_profile(ez)
                ratio  = max(0.0, 1.0 - (ex / hw_t) ** 2) if hw_t else 0.0
                y_surf = hd_t * math.sqrt(ratio) + 0.004
                ring.append((ex, y_surf + thick, ez))   # outer
                ring.append((ex, y_surf,         ez))   # inner
            rings.append([ring[0], ring[1], ring[3], ring[2]])

        bm  = ring_prism_mesh(rings)
        obj = bm_to_object(bm, name, (0, 0, 0))
        mat(obj, "belt")
        finalize_organic(obj, levels=1)
        return obj

    sw = TORSO_CROSS[0][1]
    parts.append(belt("Belt_RtoL",  sw * 0.78, SHOULDER_Z - 0.04,
                                   -sw * 0.70, HIP_Z      + 0.06))
    parts.append(belt("Belt_LtoR", -sw * 0.78, SHOULDER_Z - 0.04,
                                    sw * 0.70, HIP_Z      + 0.06))

    # Brass belt plate where the straps cross
    bm        = sphere_mesh(0.026, u=12, v=8, scale=(1.0, 0.40, 1.0))
    _, hd     = torso_profile(TORSO_Z + 0.05)
    belt_plate = bm_to_object(bm, "Belt_plate", (0, hd + 0.016, TORSO_Z + 0.05))
    mat(belt_plate, "brass")
    select_only(belt_plate)
    bpy.ops.object.shade_smooth()
    parts.append(belt_plate)

    return parts


# ===========================================================================
# SHAKO  —  r_base=0.095, r_top=0.125, height=0.21
# ===========================================================================

def make_shako():
    """
    Belgic false-front shako: body flares wider toward the crown,
    with a peaked visor, false-front panel, brass plate and plume.
    """
    parts  = []
    body_h = 0.21
    bot_z  = HEAD_Z + HEAD_R * 0.80
    cz     = bot_z  + body_h / 2

    bm   = tapered_cone_mesh(r_base=0.095, r_top=0.125,
                              height=body_h, segments=20)
    body = bm_to_object(bm, "Shako_body", (0, 0, cz))
    mat(body, "shako")
    finalize_organic(body, levels=2)
    parts.append(body)

    # False front
    bm_ff = bmesh.new()
    bmesh.ops.create_cube(bm_ff, size=1.0)
    bmesh.ops.scale(bm_ff, vec=Vector((0.185, 0.020, 0.072)), verts=bm_ff.verts)
    ff = bm_to_object(bm_ff, "Shako_falsefront",
                      (0, 0.112, bot_z + body_h - 0.022))
    mat(ff, "shako")
    finalize_organic(ff, levels=1)
    parts.append(ff)

    # Visor / peak — flat ellipse tilted downward at the front
    bm_pk = bmesh.new()
    bmesh.ops.create_cone(bm_pk, cap_ends=True, cap_tris=False,
                          segments=18,
                          radius1=0.132, radius2=0.132, depth=0.020)
    bmesh.ops.scale(bm_pk, vec=Vector((1.0, 1.32, 1.0)), verts=bm_pk.verts)
    peak = bm_to_object(bm_pk, "Shako_peak", (0, 0.062, bot_z + 0.012))
    peak.rotation_euler = (math.radians(-18), 0, 0)
    mat(peak, "shako")
    finalize_organic(peak, levels=1)
    parts.append(peak)

    # Brass plate
    bm_pl = bmesh.new()
    bmesh.ops.create_cube(bm_pl, size=1.0)
    bmesh.ops.scale(bm_pl, vec=Vector((0.088, 0.012, 0.078)), verts=bm_pl.verts)
    plate = bm_to_object(bm_pl, "Shako_plate",
                         (0, 0.122, bot_z + body_h - 0.032))
    mat(plate, "brass")
    finalize_organic(plate, levels=1)
    parts.append(plate)

    # Plume / tuft
    bm_t = tapered_cone_mesh(r_base=0.026, r_top=0.016,
                              height=0.10, segments=12)
    tuft = bm_to_object(bm_t, "Shako_tuft",
                        (0, 0.018, bot_z + body_h + 0.05))
    mat(tuft, "facing")
    finalize_organic(tuft, levels=1)
    parts.append(tuft)

    return parts


# ===========================================================================
# MUSKET  (Brown Bess / Pattern 1796-1816 flintlock, with socket bayonet)
# ===========================================================================
#
# Built in a LOCAL frame where the musket axis runs along +Z:
#   local z = 0.00  -> butt (shoulder end)
#   local z grows toward the muzzle / bayonet tip
# The whole weapon is held vertically at the soldier's right side, so the
# local +Z already maps to world +Z — only a positional offset is applied
# via `base`. No global rotation needed.

def make_musket():
    parts = []

    # --- placement of the local frame in world space ---
    MUSKET_X = -(TORSO_CROSS[0][1] + 0.08)   # right side of the body  (≈ -0.31)
    MUSKET_Y = 0.06                          # slightly in front of the soldier
    BUTT_Z   = HIP_Z - 0.10                  # butt sits by the right hand
    base     = Vector((MUSKET_X, MUSKET_Y, BUTT_Z))

    def loc(x, y, z):
        return (base.x + x, base.y + y, base.z + z)

    def add(obj, key):
        mat(obj, key)
        finalize_organic(obj, levels=1)
        parts.append(obj)
        return obj

    # gentle curvature of the wooden stock along its length (Y centre offset)
    def stock_y(z):
        pts = [(0.00, 0.020), (0.24, 0.000), (1.00, -0.010)]
        if z <= pts[0][0]:  return pts[0][1]
        if z >= pts[-1][0]: return pts[-1][1]
        for i in range(len(pts) - 1):
            z0, v0 = pts[i]
            z1, v1 = pts[i + 1]
            if z0 <= z <= z1:
                t = (z - z0) / (z1 - z0)
                return v0 + (v1 - v0) * t
        return pts[-1][1]

    # -----------------------------------------------------------------
    # STOCK  (one ring_prism_mesh, butt -> fore-end, 10 oval sections)
    # -----------------------------------------------------------------
    # (local_z, half_width_x, half_depth_y)
    stock_sects = [
        (0.00, 0.060, 0.045),   # butt face
        (0.08, 0.060, 0.045),   # butt
        (0.18, 0.040, 0.030),   # toward the wrist
        (0.24, 0.030, 0.025),   # wrist / grip — narrowest
        (0.30, 0.038, 0.028),   # behind the lock
        (0.42, 0.035, 0.026),   # lock area
        (0.50, 0.030, 0.024),   # start of the fore-end
        (0.72, 0.026, 0.022),   # fore-end
        (0.88, 0.024, 0.020),   # fore-end
        (1.00, 0.022, 0.018),   # fore-end cap
    ]
    rings = [ellipse_ring(0.0, stock_y(z), z, rx, ry, n=12)
             for z, rx, ry in stock_sects]
    stock = bm_to_object(ring_prism_mesh(rings), "Musket_stock", base)
    add(stock, "wood")

    # -----------------------------------------------------------------
    # BARREL  (round, tapering breech -> muzzle, sits above the fore-end)
    # -----------------------------------------------------------------
    barrel_breech = 0.30
    barrel_len    = 1.07
    barrel_y      = 0.015                       # offset above the stock axis
    barrel_cz     = barrel_breech + barrel_len / 2
    muzzle_z      = barrel_breech + barrel_len  # 1.37
    bm = tapered_cone_mesh(r_base=0.016, r_top=0.013,
                           height=barrel_len, segments=14)
    barrel = bm_to_object(bm, "Musket_barrel", loc(0, barrel_y, barrel_cz))
    add(barrel, "steel")

    # Muzzle cap
    cap = bm_to_object(sphere_mesh(0.018, u=12, v=8),
                       "Musket_muzzlecap", loc(0, barrel_y, muzzle_z))
    add(cap, "steel")

    # -----------------------------------------------------------------
    # RAMROD  (under the barrel, in the fore-end channel)
    # -----------------------------------------------------------------
    bm = tapered_cone_mesh(r_base=0.005, r_top=0.004, height=0.90, segments=8)
    ramrod = bm_to_object(bm, "Musket_ramrod",
                          loc(0, barrel_y - 0.008, 0.45 + 0.45))
    add(ramrod, "steel")

    # -----------------------------------------------------------------
    # BARREL BANDS  (3 metal rings clasping barrel + fore-end)
    # -----------------------------------------------------------------
    for i, bz in enumerate((0.42, 0.62, 0.82)):
        bm = tapered_cone_mesh(r_base=0.028, r_top=0.028,
                               height=0.018, segments=16)
        band = bm_to_object(bm, f"Musket_band{i}", loc(0, barrel_y, bz))
        add(band, "steel")

    # -----------------------------------------------------------------
    # FLINTLOCK  (lock plate + cock/hammer + frizzen, right side)
    # -----------------------------------------------------------------
    lock_x = -0.045   # right side of the stock
    # (a) lock plate — flat plate against the stock
    bm = box_mesh(0.018, 0.10, 0.055)
    plate = bm_to_object(bm, "Lock_plate", loc(lock_x, 0.020, 0.28))
    add(plate, "steel")

    # (b) cock / hammer — tumbler base + arm cocked back & up
    bm = box_mesh(0.015, 0.020, 0.035)
    tumbler = bm_to_object(bm, "Lock_tumbler", loc(lock_x - 0.006, 0.040, 0.300))
    add(tumbler, "steel")

    bm = tapered_cone_mesh(r_base=0.008, r_top=0.005, height=0.06, segments=8)
    cock = bm_to_object(bm, "Lock_cock", loc(lock_x - 0.006, 0.058, 0.330))
    cock.rotation_euler = (-0.8, 0, 0)          # drawn back, ready to fire
    add(cock, "steel")

    # (c) frizzen — angled forward, just ahead of the cock
    bm = box_mesh(0.015, 0.030, 0.042)
    frizzen = bm_to_object(bm, "Lock_frizzen", loc(lock_x - 0.004, 0.072, 0.352))
    frizzen.rotation_euler = (math.radians(15), 0, 0)
    add(frizzen, "steel")

    # -----------------------------------------------------------------
    # TRIGGER GUARD  (bowed metal tube under the wrist)
    # -----------------------------------------------------------------
    guard_rings = []
    g_steps = 8
    for s in range(g_steps + 1):
        t   = s / g_steps
        gz  = 0.20 + 0.13 * t
        gy  = 0.030 + 0.030 * math.sin(math.pi * t)   # bows downward/outward
        gx  = -0.010
        ring = []
        for k in range(6):
            a = 2 * math.pi * k / 6
            ring.append((gx + 0.004 * math.cos(a),
                         gy + 0.004 * math.sin(a),
                         gz))
        guard_rings.append(ring)
    guard = bm_to_object(ring_prism_mesh(guard_rings), "Trigger_guard", base)
    add(guard, "steel")

    # -----------------------------------------------------------------
    # BUTT PLATE  (metal plate over the shoulder end of the butt)
    # -----------------------------------------------------------------
    bm = box_mesh(0.062, 0.008, 0.10)
    buttplate = bm_to_object(bm, "Butt_plate", loc(0, 0.020 - 0.044, 0.05))
    add(buttplate, "steel")

    # -----------------------------------------------------------------
    # SOCKET BAYONET  (socket + offset elbow + triangular blade)
    # -----------------------------------------------------------------
    # (a) socket tube clamping over the muzzle
    bm = tapered_cone_mesh(r_base=0.020, r_top=0.020, height=0.06, segments=12)
    socket = bm_to_object(bm, "Bayonet_socket",
                          loc(0, barrel_y, muzzle_z + 0.03))
    add(socket, "steel")

    # (b) elbow / offset arm carrying the blade to the side of the bore
    bm = box_mesh(0.015, 0.040, 0.015)
    elbow = bm_to_object(bm, "Bayonet_elbow",
                         loc(0, barrel_y + 0.028, muzzle_z + 0.065))
    add(elbow, "steel")

    # (c) triangular blade, tapering to a point (length ≈ 0.43 m)
    blade_y0 = barrel_y + 0.030
    blade_z0 = muzzle_z + 0.07
    blade_len = 0.43
    blade_sects = [
        (0.00, 0.013),
        (0.06, 0.012),
        (0.18, 0.009),
        (0.30, 0.006),
        (0.40, 0.003),
        (blade_len, 0.001),
    ]
    blade_rings = []
    for dz, rr in blade_sects:
        ring = []
        for k in range(3):                       # 3-sided cross-section
            a = math.pi / 2 + 2 * math.pi * k / 3
            ring.append((rr * math.cos(a),
                         blade_y0 + rr * math.sin(a),
                         blade_z0 + dz))
        blade_rings.append(ring)
    blade = bm_to_object(ring_prism_mesh(blade_rings), "Bayonet_blade", base)
    add(blade, "steel")

    return parts


# ===========================================================================
# Assembly
# ===========================================================================

def assemble_soldier():
    clear_scene()
    build_materials()

    parts = []
    parts.append(make_torso())
    parts.extend(make_coat_tails())
    parts.extend(make_lapels())
    parts.append(make_collar())
    parts.append(make_neck())
    parts.append(make_head())
    parts.extend(make_shako())
    parts.extend(make_arm('L'))
    parts.extend(make_arm('R'))
    parts.extend(make_leg('L'))
    parts.extend(make_leg('R'))
    parts.extend(make_crossbelts())
    parts.extend(make_musket())

    # Bake any pending rotation transforms (musket tilt, peak tilt …)
    bpy.ops.object.select_all(action='DESELECT')
    for o in parts:
        o.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)

    soldier = join_objects(parts, "BP_BritishInfantry")

    bpy.context.scene.cursor.location = (0.0, 0.0, 0.0)
    select_only(soldier)
    bpy.ops.object.origin_set(type='ORIGIN_CURSOR')

    nv = len(soldier.data.vertices)
    nf = len(soldier.data.polygons)
    print(f"[BritInf] BP_BritishInfantry — {nv} verts, {nf} faces (high-detail)")
    return soldier


if __name__ == "__main__":
    assemble_soldier()
    print("Done! File -> Export -> FBX for UE5 import.")
    print("UE5 import: Combine Meshes=True, Scale=1.0, Z Up, -Y Forward.")
    print("Note: high-detail — decimate / build LODs before HISM instancing.")
