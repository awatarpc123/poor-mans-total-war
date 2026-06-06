"""
Poor Man's Total War - British Line Infantry Generator (High-Detail)
Run in Blender (Scripting tab) to generate a Napoleonic-era British infantryman.

Usage:
  1. Open Blender -> Scripting tab
  2. Open this file (or paste it)
  3. Press "Run Script" (Alt+P)
  4. File -> Export -> FBX -> import into UE5

This is the HIGH-DETAIL pass: polycount and topology are intentionally NOT
optimised. Organic / cloth parts use dense primitives + an applied
Subdivision Surface modifier (level 2) and smooth shading for natural,
rounded forms. Spherical joints (shoulders, elbows, hips, knees) bridge the
limb segments so the subdivided surface flows like fabric over a body
instead of looking like cut-off cylinders.

Coordinate convention: Z up, Z=0 at the ground, model faces +Y.
Reference silhouette: British line infantry, c.1800-1815 (red coat with
facings/lapels, white cross-belts, grey trousers, black gaiters, Belgic shako).
"""

import bpy
import bmesh
from mathutils import Vector
import math


# ===========================================================================
# Low-level helpers
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
    mat = bpy.data.materials.new(name=name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (r, g, b, 1.0)
    bsdf.inputs["Roughness"].default_value = rough
    bsdf.inputs["Metallic"].default_value = metal
    return mat


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
    """Shade smooth + add and APPLY a Subdivision Surface modifier."""
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


# --- primitive builders ----------------------------------------------------

def tapered_cone_mesh(r_base, r_top, height, segments=16):
    """Tapered cylinder (r_base at -height/2, r_top at +height/2)."""
    bm = bmesh.new()
    bmesh.ops.create_cone(
        bm, cap_ends=True, cap_tris=False,
        segments=segments, radius1=r_base, radius2=r_top, depth=height,
    )
    return bm


def sphere_mesh(radius, u=20, v=12, scale=(1.0, 1.0, 1.0)):
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=u, v_segments=v, radius=radius)
    if scale != (1.0, 1.0, 1.0):
        bmesh.ops.scale(bm, vec=Vector(scale), verts=bm.verts)
    return bm


def box_mesh(sx, sy, sz):
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector((sx, sy, sz)), verts=bm.verts)
    return bm


def ring_prism_mesh(rings):
    """
    Closed prism from a list of cross-section rings.
    Each ring is a list of (x, y, z) tuples; all rings share the same count.
    First ring = start cap, last ring = end cap.
    """
    bm = bmesh.new()
    n = len(rings[0])
    vert_rings = []
    for ring in rings:
        vert_rings.append([bm.verts.new(Vector(p)) for p in ring])
    bm.verts.ensure_lookup_table()

    for ri in range(len(vert_rings) - 1):
        a, b = vert_rings[ri], vert_rings[ri + 1]
        for i in range(n):
            j = (i + 1) % n
            bm.faces.new([a[i], a[j], b[j], b[i]])

    bm.faces.new(vert_rings[0])
    bm.faces.new(list(reversed(vert_rings[-1])))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    return bm


# ===========================================================================
# Materials (Napoleonic British line infantry palette)
# ===========================================================================
MAT = {}


def build_materials():
    MAT["coat"]     = make_material("BritInf_Coat",     0.58, 0.05, 0.05)  # scarlet
    MAT["facing"]   = make_material("BritInf_Facing",   0.90, 0.88, 0.82)  # buff/white lapels & cuffs
    MAT["trousers"] = make_material("BritInf_Trousers", 0.62, 0.62, 0.60)  # grey overalls
    MAT["gaiter"]   = make_material("BritInf_Gaiter",   0.07, 0.07, 0.07)  # black gaiters
    MAT["shako"]    = make_material("BritInf_Shako",    0.05, 0.05, 0.05)  # black felt
    MAT["skin"]     = make_material("BritInf_Skin",     0.80, 0.61, 0.47)
    MAT["brass"]    = make_material("BritInf_Brass",    0.74, 0.56, 0.11, rough=0.35, metal=0.9)
    MAT["belt"]     = make_material("BritInf_Crossbelt",0.93, 0.91, 0.86)  # pipe-clayed white
    MAT["boot"]     = make_material("BritInf_Boot",     0.06, 0.06, 0.06)
    MAT["wood"]     = make_material("Musket_Wood",      0.34, 0.19, 0.09)
    MAT["steel"]    = make_material("Musket_Steel",     0.30, 0.31, 0.33, rough=0.3, metal=0.9)


def mat(obj, key):
    obj.data.materials.append(MAT[key])
    return obj


# ===========================================================================
# Skeleton landmarks (metres, Z=0 at ground)
# ===========================================================================
FOOT_SZ     = 0.060
LOWER_LEG_H = 0.40
UPPER_LEG_H = 0.40
TORSO_H     = 0.50
NECK_H      = 0.085
HEAD_R      = 0.115

ANKLE_Z    = FOOT_SZ
KNEE_Z     = ANKLE_Z + LOWER_LEG_H          # 0.46
HIP_Z      = KNEE_Z + UPPER_LEG_H           # 0.86
TORSO_Z    = HIP_Z + TORSO_H / 2            # 1.11
SHOULDER_Z = HIP_Z + TORSO_H                # 1.36
NECK_Z     = SHOULDER_Z + NECK_H / 2        # 1.4025
HEAD_Z     = SHOULDER_Z + NECK_H + HEAD_R * 0.85   # ~1.543

# Torso cross-section half-extents at each height (for profile sampling)
# (z_world, half_width, half_depth)
TORSO_PROFILE = [
    (SHOULDER_Z,        0.205, 0.135),   # shoulders (broad)
    (SHOULDER_Z - 0.13, 0.190, 0.130),   # chest
    (TORSO_Z,           0.160, 0.115),   # waist (cinched)
    (HIP_Z + 0.04,      0.180, 0.120),   # hips
    (HIP_Z,             0.175, 0.118),   # hip bottom
]

HIP_OFFSET_X = 0.095
UPPER_ARM_H  = 0.28
LOWER_ARM_H  = 0.25


def torso_profile(z):
    """Interpolate (half_width, half_depth) of the torso at world Z."""
    pts = TORSO_PROFILE
    if z >= pts[0][0]:
        return pts[0][1], pts[0][2]
    if z <= pts[-1][0]:
        return pts[-1][1], pts[-1][2]
    for i in range(len(pts) - 1):
        z0, w0, d0 = pts[i]
        z1, w1, d1 = pts[i + 1]
        if z1 <= z <= z0:
            t = (z - z0) / (z1 - z0)
            return w0 + (w1 - w0) * t, d0 + (d1 - d0) * t
    return pts[-1][1], pts[-1][2]


# ===========================================================================
# Torso, coat tails, lapels, epaulettes
# ===========================================================================

def make_torso():
    """Dense trapezoid prism following TORSO_PROFILE; subdivided into a body."""
    rings = []
    seg = 12  # angular resolution of the cross-section
    for z, hw, hd in TORSO_PROFILE:
        ring = []
        for k in range(seg):
            a = 2 * math.pi * k / seg
            # Rounded-rectangle-ish cross section via ellipse
            ring.append((hw * math.cos(a), hd * math.sin(a), z))
        rings.append(ring)
    bm = ring_prism_mesh(rings)
    obj = bm_to_object(bm, "Torso", (0, 0, 0))
    mat(obj, "coat")
    finalize_organic(obj, levels=2)
    return obj


def make_coat_tails():
    """
    Rear & side coat tails hanging from the waist down over the thighs,
    with the classic turned-back fronts (white facing inside edge).
    """
    parts = []
    top_z = HIP_Z + 0.06
    bot_z = KNEE_Z + 0.10          # falls to just above the knee
    back_y = -torso_profile(HIP_Z)[1] - 0.005

    # Two main rear tails (left & right of centre back)
    for sign in (+1, -1):
        x_in  = sign * 0.02
        x_out = sign * 0.16
        th    = 0.018
        rings = []
        # top (at waist, tucked in) -> bottom (flared out)
        steps = 4
        for s in range(steps + 1):
            t = s / steps
            z = top_z + (bot_z - top_z) * t
            flare = 1.0 + 0.45 * t          # tails splay outward at the hem
            y = back_y - 0.02 * t           # drift backward toward the hem
            xi = x_in * flare
            xo = x_out * flare
            rings.append([
                (xi, y + th, z),
                (xo, y + th, z),
                (xo, y - th, z),
                (xi, y - th, z),
            ])
        bm = ring_prism_mesh(rings)
        tail = bm_to_object(bm, f"CoatTail_{'L' if sign>0 else 'R'}", (0, 0, 0))
        mat(tail, "coat")
        finalize_organic(tail, levels=2)
        parts.append(tail)

    # Side skirt panels (cover the hips, blending the coat over the thighs)
    for sign in (+1, -1):
        hw, hd = torso_profile(HIP_Z)
        cx = sign * (hw - 0.01)
        th = 0.02
        rings = []
        steps = 4
        for s in range(steps + 1):
            t = s / steps
            z = top_z + (bot_z + 0.02 - top_z) * t
            depth = hd * (1.0 + 0.15 * t)
            rings.append([
                (cx + sign * th, depth, z),
                (cx + sign * th, -depth, z),
                (cx - sign * th, -depth, z),
                (cx - sign * th, depth, z),
            ])
        bm = ring_prism_mesh(rings)
        skirt = bm_to_object(bm, f"CoatSkirt_{'L' if sign>0 else 'R'}", (0, 0, 0))
        mat(skirt, "coat")
        finalize_organic(skirt, levels=2)
        parts.append(skirt)

    return parts


def make_lapels():
    """
    Two coloured lapels (plastron) on the chest front + a row of brass buttons.
    The lapels sit proud of the coat and flare open toward the shoulders.
    """
    parts = []
    z_top = SHOULDER_Z - 0.03
    z_bot = TORSO_Z + 0.02

    for sign in (+1, -1):
        rings = []
        steps = 3
        for s in range(steps + 1):
            t = s / steps
            z = z_top + (z_bot - z_top) * t
            hw, hd = torso_profile(z)
            # wider apart at the top (open V), closer at the waist
            x_in  = sign * (0.015 + 0.005 * (1 - t))
            x_out = sign * (0.085 - 0.015 * t)
            y_surf = hd + 0.004
            th = 0.012
            rings.append([
                (x_in,  y_surf + th, z),
                (x_out, y_surf + th, z),
                (x_out, y_surf,      z),
                (x_in,  y_surf,      z),
            ])
        bm = ring_prism_mesh(rings)
        lap = bm_to_object(bm, f"Lapel_{'L' if sign>0 else 'R'}", (0, 0, 0))
        mat(lap, "facing")
        finalize_organic(lap, levels=2)
        parts.append(lap)

    # Brass button rows (down each lapel inner edge)
    for sign in (+1, -1):
        for s in range(6):
            t = s / 5
            z = z_top + (z_bot - z_top) * t
            hw, hd = torso_profile(z)
            bm = sphere_mesh(0.011, u=10, v=8)
            btn = bm_to_object(bm, "Button", (sign * 0.02, hd + 0.018, z))
            mat(btn, "brass")
            select_only(btn)
            bpy.ops.object.shade_smooth()
            parts.append(btn)

    return parts


def make_collar():
    """Standing collar (facing colour) wrapping the neck base."""
    z = SHOULDER_Z + 0.01
    bm = tapered_cone_mesh(r_base=0.085, r_top=0.075, height=0.07, segments=16)
    obj = bm_to_object(bm, "Collar", (0, 0.01, z + 0.02))
    mat(obj, "facing")
    finalize_organic(obj, levels=1)
    return obj


# ===========================================================================
# Head / neck / shako
# ===========================================================================

def make_neck():
    bm = tapered_cone_mesh(r_base=0.058, r_top=0.046, height=NECK_H + 0.03, segments=14)
    obj = bm_to_object(bm, "Neck", (0, 0, NECK_Z))
    mat(obj, "skin")
    finalize_organic(obj, levels=2)
    return obj


def make_head():
    """
    Sphere reshaped into a head: narrowed at the crown, extruded jaw/chin
    at the bottom-front so the face reads with a jawline rather than a ball.
    """
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=20, v_segments=16, radius=HEAD_R)
    # Slightly narrow the skull front-back, keep height
    bmesh.ops.scale(bm, vec=Vector((0.95, 0.92, 1.05)), verts=bm.verts)

    # Sculpt a jaw: pull lower-front verts forward (+Y) and inward toward a chin
    for vrt in bm.verts:
        co = vrt.co
        if co.z < -0.02:                       # lower half of the head
            jaw_t = min(1.0, (-co.z) / HEAD_R)  # 0 at mid, 1 at bottom
            if co.y > 0:                       # front-facing -> build the chin
                co.y += 0.025 * jaw_t
            # taper the jaw inward in X so it narrows to a chin
            co.x *= (1.0 - 0.18 * jaw_t)
            co.z *= (1.0 + 0.05 * jaw_t)       # lengthen the jaw downward a touch
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)

    obj = bm_to_object(bm, "Head", (0, 0, HEAD_Z))
    mat(obj, "skin")
    finalize_organic(obj, levels=2)
    return obj


def make_shako():
    """
    Belgic ('false-front') shako: cylindrical felt body flaring WIDER at the
    top, a flat raised false front, a peaked leather visor at the front, plus
    a brass plate, cords and a tuft/plume.
    """
    parts = []
    body_h = 0.20
    bot_z  = HEAD_Z + HEAD_R * 0.78
    cz     = bot_z + body_h / 2

    # Body — narrower at the head, wider at the crown (the signature flare)
    bm = tapered_cone_mesh(r_base=0.098, r_top=0.120, height=body_h, segments=20)
    body = bm_to_object(bm, "Shako_body", (0, 0, cz))
    mat(body, "shako")
    finalize_organic(body, levels=2)
    parts.append(body)

    # Raised false front (Belgic plate backing) at the top front
    bm_ff = box_mesh(0.18, 0.02, 0.07)
    ff = bm_to_object(bm_ff, "Shako_falsefront", (0, 0.108, bot_z + body_h - 0.02))
    mat(ff, "shako")
    finalize_organic(ff, levels=1)
    parts.append(ff)

    # Peak / visor — a flattened, downward-angled disc segment at the front
    bm_peak = bmesh.new()
    bmesh.ops.create_cone(bm_peak, cap_ends=True, cap_tris=False,
                          segments=18, radius1=0.135, radius2=0.135, depth=0.022)
    bmesh.ops.scale(bm_peak, vec=Vector((1.0, 1.25, 1.0)), verts=bm_peak.verts)
    peak = bm_to_object(bm_peak, "Shako_peak", (0, 0.06, bot_z + 0.012))
    peak.rotation_euler = (math.radians(-16), 0, 0)
    mat(peak, "shako")
    finalize_organic(peak, levels=1)
    parts.append(peak)

    # Brass plate on the false front
    bm_plate = box_mesh(0.085, 0.012, 0.075)
    plate = bm_to_object(bm_plate, "Shako_plate", (0, 0.122, bot_z + body_h - 0.03))
    mat(plate, "brass")
    finalize_organic(plate, levels=1)
    parts.append(plate)

    # Tuft / plume on top
    bm_tuft = tapered_cone_mesh(r_base=0.028, r_top=0.018, height=0.10, segments=12)
    tuft = bm_to_object(bm_tuft, "Shako_tuft", (0, 0.02, bot_z + body_h + 0.05))
    mat(tuft, "facing")
    finalize_organic(tuft, levels=1)
    parts.append(tuft)

    return parts


# ===========================================================================
# Arms (with spherical shoulder/elbow joints, cuffs, epaulettes, hands)
# ===========================================================================

def make_arm(side):
    parts = []
    sign  = 1 if side == 'L' else -1
    arm_x = sign * (TORSO_PROFILE[0][1] + 0.03)

    shoulder_z = SHOULDER_Z - 0.01
    elbow_z    = shoulder_z - UPPER_ARM_H
    wrist_z    = elbow_z - LOWER_ARM_H

    # --- Shoulder ball joint (fills the deltoid, blends into torso) ---
    bm = sphere_mesh(0.072, u=18, v=12)
    shoulder = bm_to_object(bm, f"ShoulderJoint_{side}", (arm_x, 0, shoulder_z))
    mat(shoulder, "coat")
    finalize_organic(shoulder, levels=2)
    parts.append(shoulder)

    # --- Upper arm (tapered) ---
    bm = tapered_cone_mesh(r_base=0.050, r_top=0.064, height=UPPER_ARM_H, segments=16)
    uarm = bm_to_object(bm, f"UpperArm_{side}", (arm_x, 0, shoulder_z - UPPER_ARM_H / 2))
    mat(uarm, "coat")
    finalize_organic(uarm, levels=2)
    parts.append(uarm)

    # --- Elbow ball joint ---
    bm = sphere_mesh(0.050, u=16, v=10)
    elbow = bm_to_object(bm, f"ElbowJoint_{side}", (arm_x, 0, elbow_z))
    mat(elbow, "coat")
    finalize_organic(elbow, levels=2)
    parts.append(elbow)

    # --- Lower arm (tapered) ---
    bm = tapered_cone_mesh(r_base=0.036, r_top=0.048, height=LOWER_ARM_H, segments=16)
    larm = bm_to_object(bm, f"LowerArm_{side}", (arm_x, 0, elbow_z - LOWER_ARM_H / 2))
    mat(larm, "coat")
    finalize_organic(larm, levels=2)
    parts.append(larm)

    # --- Cuff (thick facing-colour band at the wrist) ---
    bm = tapered_cone_mesh(r_base=0.052, r_top=0.046, height=0.06, segments=16)
    cuff = bm_to_object(bm, f"Cuff_{side}", (arm_x, 0, wrist_z + 0.03))
    mat(cuff, "facing")
    finalize_organic(cuff, levels=2)
    parts.append(cuff)

    # --- Epaulette / shoulder strap (rolled fabric over the shoulder top) ---
    bm = sphere_mesh(0.055, u=14, v=10, scale=(1.4, 0.7, 0.5))
    ep = bm_to_object(bm, f"Epaulette_{side}", (arm_x, 0, shoulder_z + 0.055))
    mat(ep, "facing")
    finalize_organic(ep, levels=2)
    parts.append(ep)
    # brass wing tuft at the outer edge of the epaulette
    bm = sphere_mesh(0.02, u=10, v=8)
    tuft = bm_to_object(bm, f"EpTuft_{side}", (arm_x + sign * 0.05, 0, shoulder_z + 0.05))
    mat(tuft, "brass")
    select_only(tuft); bpy.ops.object.shade_smooth()
    parts.append(tuft)

    # --- Hand (sphere flattened into a mitten/fist) ---
    bm = sphere_mesh(0.045, u=14, v=10, scale=(0.85, 0.7, 1.25))
    hand = bm_to_object(bm, f"Hand_{side}", (arm_x, 0, wrist_z - 0.05))
    mat(hand, "skin")
    finalize_organic(hand, levels=2)
    parts.append(hand)

    return parts


# ===========================================================================
# Legs (hip/knee ball joints, thigh, shin/gaiter, boot)
# ===========================================================================

def make_leg(side):
    parts = []
    sign  = 1 if side == 'L' else -1
    hip_x = sign * HIP_OFFSET_X

    # --- Hip / pelvis ball joint ---
    bm = sphere_mesh(0.085, u=18, v=12)
    hip = bm_to_object(bm, f"HipJoint_{side}", (hip_x, 0, HIP_Z - 0.02))
    mat(hip, "trousers")
    finalize_organic(hip, levels=2)
    parts.append(hip)

    # --- Thigh (tapered: wide at hip, narrow at knee) ---
    bm = tapered_cone_mesh(r_base=0.058, r_top=0.088, height=UPPER_LEG_H, segments=16)
    thigh = bm_to_object(bm, f"Thigh_{side}", (hip_x, 0, KNEE_Z + UPPER_LEG_H / 2))
    mat(thigh, "trousers")
    finalize_organic(thigh, levels=2)
    parts.append(thigh)

    # --- Knee ball joint ---
    bm = sphere_mesh(0.055, u=16, v=10)
    knee = bm_to_object(bm, f"KneeJoint_{side}", (hip_x, 0, KNEE_Z))
    mat(knee, "trousers")
    finalize_organic(knee, levels=2)
    parts.append(knee)

    # --- Shin / gaiter (black, tapered to ankle) ---
    bm = tapered_cone_mesh(r_base=0.040, r_top=0.062, height=LOWER_LEG_H, segments=16)
    shin = bm_to_object(bm, f"Shin_{side}", (hip_x, 0, ANKLE_Z + LOWER_LEG_H / 2))
    mat(shin, "gaiter")
    finalize_organic(shin, levels=2)
    parts.append(shin)

    # --- Boot ---
    parts.append(make_boot(side))
    return parts


def make_boot(side):
    """Rounded boot: a 6-section prism (heel->ball->toe) subdivided smooth."""
    sign  = 1 if side == 'L' else -1
    hip_x = sign * HIP_OFFSET_X

    hw, fw = 0.040, 0.046
    heel_y, ball_y, toe_y = -0.035, 0.020, 0.125
    top_z, sole_z = FOOT_SZ / 2, -FOOT_SZ / 2

    cross = [
        (-hw, heel_y),
        ( hw, heel_y),
        ( fw, ball_y),
        ( fw * 0.6, toe_y * 0.85),
        ( 0, toe_y),
        (-fw * 0.6, toe_y * 0.85),
        (-fw, ball_y),
    ]
    top = [(x, y, top_z) for x, y in cross]
    bot = [(x, y, sole_z) for x, y in cross]
    bm = ring_prism_mesh([bot, top])

    obj = bm_to_object(bm, f"Boot_{side}", (hip_x, 0.0, ANKLE_Z - FOOT_SZ / 2))
    mat(obj, "boot")
    finalize_organic(obj, levels=2)
    return obj


# ===========================================================================
# Cross-belts (conform to the chest curvature)
# ===========================================================================

def make_crossbelts():
    """
    Two white belts running shoulder->opposite hip, each vertex projected
    onto the torso's elliptical surface so the strap hugs the chest instead
    of floating as a flat box.
    """
    parts = []

    def belt(name, x0, z0, x1, z1, width=0.05, thick=0.014):
        steps = 16
        centres = []
        for s in range(steps + 1):
            t = s / steps
            x = x0 + (x1 - x0) * t
            z = z0 + (z1 - z0) * t
            centres.append((x, z))

        rings = []
        for i, (x, z) in enumerate(centres):
            # travel direction in the X-Z plane
            if i < len(centres) - 1:
                dx = centres[i + 1][0] - x
                dz = centres[i + 1][1] - z
            else:
                dx = x - centres[i - 1][0]
                dz = z - centres[i - 1][1]
            L = math.hypot(dx, dz) or 1e-6
            # perpendicular (width) axis in X-Z
            px, pz = -dz / L, dx / L
            hw_belt = width / 2

            ring = []
            for wsign in (+1, -1):
                ex = x + px * hw_belt * wsign
                ez = z + pz * hw_belt * wsign
                half_w, half_d = torso_profile(ez)
                # project X onto the ellipse to find the front surface Y
                ratio = max(0.0, 1.0 - (ex / half_w) ** 2) if half_w else 0.0
                y_surf = half_d * math.sqrt(ratio) + 0.006
                ring.append((ex, y_surf + thick, ez))   # outer face
                ring.append((ex, y_surf,         ez))   # inner face
            # reorder to a consistent 4-vert loop: outer+, inner+, inner-, outer-
            ordered = [ring[0], ring[1], ring[3], ring[2]]
            rings.append(ordered)

        bm = ring_prism_mesh(rings)
        obj = bm_to_object(bm, name, (0, 0, 0))
        mat(obj, "belt")
        finalize_organic(obj, levels=1)
        return obj

    sw = TORSO_PROFILE[0][1]
    parts.append(belt("Belt_RtoL",  sw * 0.8, SHOULDER_Z - 0.02,
                                    -sw * 0.7, HIP_Z + 0.06))
    parts.append(belt("Belt_LtoR", -sw * 0.8, SHOULDER_Z - 0.02,
                                     sw * 0.7, HIP_Z + 0.06))

    # Brass belt-plate where the belts cross
    bm = sphere_mesh(0.03, u=14, v=10, scale=(1.0, 0.4, 1.0))
    _, hd = torso_profile(TORSO_Z + 0.05)
    plate = bm_to_object(bm, "Belt_plate", (0, hd + 0.02, TORSO_Z + 0.05))
    mat(plate, "brass")
    select_only(plate); bpy.ops.object.shade_smooth()
    parts.append(plate)

    return parts


# ===========================================================================
# Musket
# ===========================================================================

def make_musket():
    parts = []
    tilt     = math.radians(9)
    mx       = -(TORSO_PROFILE[0][1] + 0.075)
    my       = 0.05
    barrel_z = TORSO_Z + 0.02

    bm = tapered_cone_mesh(r_base=0.016, r_top=0.013, height=1.20, segments=12)
    barrel = bm_to_object(bm, "Musket_barrel", (mx, my, barrel_z))
    barrel.rotation_euler = (tilt, 0, 0)
    mat(barrel, "steel")
    finalize_organic(barrel, levels=1)
    parts.append(barrel)

    bm = box_mesh(0.038, 0.055, 0.64)
    stock = bm_to_object(bm, "Musket_stock", (mx, my, barrel_z - 0.55))
    stock.rotation_euler = (tilt, 0, 0)
    mat(stock, "wood")
    finalize_organic(stock, levels=1)
    parts.append(stock)

    bm = tapered_cone_mesh(r_base=0.006, r_top=0.001, height=0.30, segments=6)
    bayonet = bm_to_object(bm, "Musket_bayonet", (mx, my, barrel_z + 0.66))
    bayonet.rotation_euler = (tilt, 0, 0)
    mat(bayonet, "steel")
    finalize_organic(bayonet, levels=1)
    parts.append(bayonet)

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

    # Bake any remaining rotations
    bpy.ops.object.select_all(action='DESELECT')
    for o in parts:
        o.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)

    soldier = join_objects(parts, "BP_BritishInfantry")

    bpy.context.scene.cursor.location = (0.0, 0.0, 0.0)
    select_only(soldier)
    bpy.ops.object.origin_set(type='ORIGIN_CURSOR')

    n_verts = len(soldier.data.vertices)
    n_faces = len(soldier.data.polygons)
    print(f"[BritInf] BP_BritishInfantry — {n_verts} verts, {n_faces} faces "
          f"(high-detail, subdivided)")
    return soldier


if __name__ == "__main__":
    assemble_soldier()
    print("Done! File -> Export -> FBX for UE5 import.")
    print("UE5 import: Combine Meshes=True, Scale=1.0, Z Up, -Y Forward.")
    print("Note: high-detail mesh — decimate / build LODs before mass instancing.")
