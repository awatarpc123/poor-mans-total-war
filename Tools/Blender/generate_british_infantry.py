"""
Poor Man's Total War - British Line Infantry Generator
Run in Blender (Scripting tab) to generate a Napoleonic-era British infantry soldier.

Usage:
  1. Open Blender -> Scripting tab
  2. Paste this script or open the file
  3. Press "Run Script" (Alt+P)
  4. File -> Export -> FBX -> import into UE5

Targets 800-1500 triangles for instanced rendering (UE5 HISM / Mass Entity).
Z=0 at feet, model faces +Y.
"""

import bpy
import bmesh
from mathutils import Vector
import math


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def clear_scene():
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    for col in list(bpy.data.collections):
        bpy.data.collections.remove(col)


def make_material(name, r, g, b):
    mat = bpy.data.materials.new(name=name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (r, g, b, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.8
    bsdf.inputs["Metallic"].default_value = 0.0
    return mat


def tapered_cone_mesh(r_base, r_top, height, segments=6):
    """
    Tapered cylinder via bmesh create_cone.
    r_base = bottom radius (local z = -height/2).
    r_top  = top    radius (local z = +height/2).
    """
    bm = bmesh.new()
    bmesh.ops.create_cone(
        bm,
        cap_ends=True,
        cap_tris=True,
        segments=segments,
        radius1=r_base,
        radius2=r_top,
        depth=height,
    )
    return bm


def quad_prism_mesh(rings):
    """
    Build a closed prism from a list of cross-section rings.
    Each ring is a list of (x, y) tuples; all rings must have the same vertex count.
    The first ring is the bottom cap, the last is the top cap.
    Returns a bmesh.
    """
    bm = bmesh.new()
    n = len(rings[0])
    vert_rings = []
    for z_verts in rings:
        row = [bm.verts.new(Vector(v)) for v in z_verts]
        vert_rings.append(row)
    bm.verts.ensure_lookup_table()

    # Side quads between consecutive rings
    for ri in range(len(vert_rings) - 1):
        r0, r1 = vert_rings[ri], vert_rings[ri + 1]
        for i in range(n):
            j = (i + 1) % n
            bm.faces.new([r0[i], r0[j], r1[j], r1[i]])

    # Caps
    bm.faces.new(vert_rings[0])
    bm.faces.new(vert_rings[-1])

    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    return bm


def bm_to_object(bm, name, location=(0, 0, 0)):
    mesh = bpy.data.meshes.new(name + "_mesh")
    bm.to_mesh(mesh)
    bm.free()
    obj = bpy.data.objects.new(name, mesh)
    obj.location = location
    bpy.context.scene.collection.objects.link(obj)
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
# Materials  (Napoleonic British palette)
# ---------------------------------------------------------------------------
MAT_COAT      = None   # scarlet red
MAT_TROUSERS  = None   # grey-white
MAT_SHAKO     = None   # black felt
MAT_SKIN      = None   # face / hands
MAT_BRASS     = None   # buttons / shako plate
MAT_CROSSBELT = None   # white leather
MAT_BOOT      = None   # black leather


def build_materials():
    global MAT_COAT, MAT_TROUSERS, MAT_SHAKO, MAT_SKIN
    global MAT_BRASS, MAT_CROSSBELT, MAT_BOOT
    MAT_COAT      = make_material("BritInf_Coat",      0.60, 0.05, 0.05)
    MAT_TROUSERS  = make_material("BritInf_Trousers",  0.85, 0.83, 0.78)
    MAT_SHAKO     = make_material("BritInf_Shako",     0.05, 0.05, 0.05)
    MAT_SKIN      = make_material("BritInf_Skin",      0.80, 0.62, 0.48)
    MAT_BRASS     = make_material("BritInf_Brass",     0.72, 0.55, 0.10)
    MAT_CROSSBELT = make_material("BritInf_Crossbelt", 0.92, 0.90, 0.85)
    MAT_BOOT      = make_material("BritInf_Boot",      0.06, 0.06, 0.06)


# ---------------------------------------------------------------------------
# Skeleton constants  (metres, Z=0 at ground)
# ---------------------------------------------------------------------------

FOOT_SZ      = 0.055   # boot total height
LOWER_LEG_H  = 0.38    # shin
UPPER_LEG_H  = 0.38    # thigh
TORSO_H      = 0.46
NECK_H       = 0.09
HEAD_R       = 0.115

# Derived Z landmarks
ANKLE_Z    = FOOT_SZ
KNEE_Z     = ANKLE_Z    + LOWER_LEG_H    # 0.435
HIP_Z      = KNEE_Z     + UPPER_LEG_H    # 0.815
TORSO_Z    = HIP_Z      + TORSO_H / 2    # 1.045  (torso centre)
SHOULDER_Z = HIP_Z      + TORSO_H        # 1.275  (top of torso)
NECK_Z     = SHOULDER_Z + NECK_H / 2     # 1.320
HEAD_Z     = SHOULDER_Z + NECK_H + HEAD_R * 0.85  # 1.488

# Torso cross-section (half extents per ring)
TORSO_W_TOP   = 0.30    # shoulder half-width
TORSO_W_WAIST = 0.20    # waist half-width
TORSO_W_HIP   = 0.24    # hip half-width
TORSO_D_TOP   = 0.14    # shoulder half-depth
TORSO_D_WAIST = 0.11
TORSO_D_HIP   = 0.12

HIP_OFFSET_X = 0.095    # lateral leg separation

UPPER_ARM_H = 0.27
LOWER_ARM_H = 0.24
HAND_H      = 0.09


# ---------------------------------------------------------------------------
# Body parts
# ---------------------------------------------------------------------------

def make_torso():
    """
    Trapezoid prism: wide shoulders, narrow waist, medium hips.
    4 rings × 4 corners = 16 verts, 12 quads + 2 caps.
    """
    half_h = TORSO_H / 2
    # Each ring entry: (half_w, half_d, local_z_offset)
    ring_defs = [
        (TORSO_W_TOP,              TORSO_D_TOP,              +half_h),          # shoulder top
        (TORSO_W_TOP * 0.93,       TORSO_D_TOP * 0.95,       +half_h * 0.35),   # upper chest
        (TORSO_W_WAIST,            TORSO_D_WAIST,            -half_h * 0.35),   # waist
        (TORSO_W_HIP,              TORSO_D_HIP,              -half_h),          # hip bottom
    ]

    # Corner order per ring (CCW from front-right when looking down +Z)
    rings = []
    for hw, hd, lz in ring_defs:
        rings.append([
            ( hw,  hd, lz),
            (-hw,  hd, lz),
            (-hw, -hd, lz),
            ( hw, -hd, lz),
        ])

    bm = quad_prism_mesh(rings)
    obj = bm_to_object(bm, "Torso", (0, 0, TORSO_Z))
    obj.data.materials.append(MAT_COAT)
    return obj


def make_neck():
    # Slightly wider at the base (collar), narrowing toward chin
    bm = tapered_cone_mesh(r_base=0.054, r_top=0.044, height=NECK_H, segments=6)
    obj = bm_to_object(bm, "Neck", (0, 0, NECK_Z))
    obj.data.materials.append(MAT_SKIN)
    return obj


def make_head():
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=8, v_segments=6, radius=HEAD_R)
    # Flatten slightly front-to-back (face is less deep than it is wide)
    bmesh.ops.scale(bm, vec=Vector((1.0, 0.82, 1.0)), verts=bm.verts)
    obj = bm_to_object(bm, "Head", (0, 0, HEAD_Z))
    obj.data.materials.append(MAT_SKIN)
    return obj


def make_shako():
    """
    Belgic shako: body slightly flared upward + brim disc + brass front plate.
    """
    parts = []
    shako_h   = 0.170
    shako_bot = HEAD_Z + HEAD_R * 0.82
    shako_z   = shako_bot + shako_h / 2

    # Body — flared (wider at top)
    bm = tapered_cone_mesh(r_base=0.100, r_top=0.108, height=shako_h, segments=10)
    body = bm_to_object(bm, "Shako_body", (0, 0, shako_z))
    body.data.materials.append(MAT_SHAKO)
    parts.append(body)

    # Brim — flat ring (slightly tapered so it reads as a disc)
    bm2 = tapered_cone_mesh(r_base=0.142, r_top=0.136, height=0.022, segments=10)
    brim = bm_to_object(bm2, "Shako_brim", (0, 0, shako_bot + 0.011))
    brim.data.materials.append(MAT_SHAKO)
    parts.append(brim)

    # Front brass plate (flat box)
    bm3 = bmesh.new()
    bmesh.ops.create_cube(bm3, size=1.0)
    bmesh.ops.scale(bm3, vec=Vector((0.058, 0.010, 0.062)), verts=bm3.verts)
    plate = bm_to_object(bm3, "Shako_plate", (0, 0.134, shako_bot + 0.068))
    plate.data.materials.append(MAT_BRASS)
    parts.append(plate)

    return parts


def make_arm(side):
    """
    Tapered arm segments:
      upper arm  — wider at shoulder (top), narrower at elbow (bottom)
      lower arm  — wider at elbow (top), narrower at wrist (bottom)
      hand       — tapered flat prism (wider at wrist, narrower at fingers)
    """
    sign  = 1 if side == 'L' else -1
    # Place arm socket slightly overlapping the torso shoulder
    arm_x = sign * (TORSO_W_TOP + 0.022)

    # ---- Upper arm ----
    ua_r_shoulder = 0.062
    ua_r_elbow    = 0.046
    arm_top_z     = SHOULDER_Z - 0.018   # top of upper arm (enters torso slightly)
    ua_center_z   = arm_top_z - UPPER_ARM_H / 2
    elbow_z       = arm_top_z - UPPER_ARM_H

    bm = tapered_cone_mesh(r_base=ua_r_elbow, r_top=ua_r_shoulder,
                           height=UPPER_ARM_H, segments=6)
    uarm = bm_to_object(bm, f"UpperArm_{side}", (arm_x, 0, ua_center_z))
    uarm.data.materials.append(MAT_COAT)

    # ---- Lower arm ----
    la_r_elbow = 0.048
    la_r_wrist = 0.032
    la_center_z = elbow_z - LOWER_ARM_H / 2

    bm2 = tapered_cone_mesh(r_base=la_r_wrist, r_top=la_r_elbow,
                            height=LOWER_ARM_H, segments=6)
    larm = bm_to_object(bm2, f"LowerArm_{side}", (arm_x, 0, la_center_z))
    larm.data.materials.append(MAT_COAT)

    # ---- Hand — tapered flat prism ----
    wrist_z  = elbow_z - LOWER_ARM_H
    hw_wrist = 0.036   # half-width at wrist
    hw_tips  = 0.028   # half-width at finger tips
    hd       = 0.020   # half palm depth

    hand_rings = [
        # Wrist (top)
        [( hw_wrist,  hd, wrist_z - 0.001),
         (-hw_wrist,  hd, wrist_z - 0.001),
         (-hw_wrist, -hd, wrist_z - 0.001),
         ( hw_wrist, -hd, wrist_z - 0.001)],
        # Finger tips (bottom)
        [( hw_tips,  hd, wrist_z - HAND_H),
         (-hw_tips,  hd, wrist_z - HAND_H),
         (-hw_tips, -hd, wrist_z - HAND_H),
         ( hw_tips, -hd, wrist_z - HAND_H)],
    ]
    bm3 = quad_prism_mesh(hand_rings)
    hand = bm_to_object(bm3, f"Hand_{side}", (arm_x, 0, 0))
    hand.data.materials.append(MAT_SKIN)

    return [uarm, larm, hand]


def make_leg(side):
    """
    Tapered leg segments:
      thigh — wider at hip (top), narrower at knee (bottom)
      shin  — wider at knee (top), narrower at ankle (bottom)
      boot  — 5-sided prism shaped like a boot
    """
    sign  = 1 if side == 'L' else -1
    hip_x = sign * HIP_OFFSET_X

    # ---- Thigh ----
    bm = tapered_cone_mesh(r_base=0.060, r_top=0.086, height=UPPER_LEG_H, segments=7)
    uleg = bm_to_object(bm, f"UpperLeg_{side}",
                        (hip_x, 0, KNEE_Z + UPPER_LEG_H / 2))
    uleg.data.materials.append(MAT_TROUSERS)

    # ---- Shin / gaiter ----
    bm2 = tapered_cone_mesh(r_base=0.038, r_top=0.064, height=LOWER_LEG_H, segments=7)
    lleg = bm_to_object(bm2, f"LowerLeg_{side}",
                        (hip_x, 0, ANKLE_Z + LOWER_LEG_H / 2))
    lleg.data.materials.append(MAT_BOOT)

    # ---- Boot ----
    boot = make_boot(side)

    return [uleg, lleg, boot]


def make_boot(side):
    """
    5-sided prism: two heel verts, two ball verts, one toe vert.
    Extends forward (+Y) in the natural walking direction.
    """
    sign  = 1 if side == 'L' else -1
    hip_x = sign * HIP_OFFSET_X

    hw     = 0.038   # heel half-width
    fw     = 0.043   # widest (ball) half-width
    heel_y = -0.030  # heel (local Y)
    ball_y =  0.022  # widest section
    toe_y  =  0.112  # toe tip

    top_z  =  FOOT_SZ / 2    # +0.0275
    sole_z = -FOOT_SZ / 2    # -0.0275

    # Cross-section vertices going CCW from heel-left
    cross = [
        (-hw, heel_y),
        ( hw, heel_y),
        ( fw, ball_y),
        (  0, toe_y),
        (-fw, ball_y),
    ]

    top_ring = [(x, y, top_z)  for x, y in cross]
    bot_ring = [(x, y, sole_z) for x, y in cross]

    bm = quad_prism_mesh([bot_ring, top_ring])
    boot_center_z = ANKLE_Z - FOOT_SZ / 2   # 0.0275

    obj = bm_to_object(bm, f"Boot_{side}", (hip_x, 0.0, boot_center_z))
    obj.data.materials.append(MAT_BOOT)
    return obj


def make_crossbelts():
    """Two diagonal white leather cross-belts on the chest front face."""
    parts = []
    belt_y = TORSO_D_TOP + 0.006

    for sign, x_off, z_tilt in [(+1, +0.04, -20), (-1, -0.04, +20)]:
        bm = bmesh.new()
        bmesh.ops.create_cube(bm, size=1.0)
        bmesh.ops.scale(bm, vec=Vector((0.026, 0.013, 0.52)), verts=bm.verts)
        name = "Belt_RS_LH" if sign > 0 else "Belt_LS_RH"
        belt = bm_to_object(bm, name, (x_off, belt_y, TORSO_Z + 0.01))
        belt.rotation_euler = (0, math.radians(13 * sign), math.radians(z_tilt))
        belt.data.materials.append(MAT_CROSSBELT)
        parts.append(belt)

    return parts


def make_musket():
    """
    Brown Bess musket held at the soldier's right side.
    Barrel tapers slightly toward the muzzle.
    """
    parts = []
    tilt     = math.radians(10)
    mx       = -(TORSO_W_TOP + 0.045)
    my       = 0.042
    barrel_z = TORSO_Z - 0.04

    # Barrel (slight taper toward muzzle)
    bm = tapered_cone_mesh(r_base=0.017, r_top=0.014, height=1.18, segments=5)
    barrel = bm_to_object(bm, "Musket_barrel", (mx, my, barrel_z))
    barrel.rotation_euler = (tilt, 0, 0)
    barrel.data.materials.append(MAT_SHAKO)
    parts.append(barrel)

    # Stock
    bm2 = bmesh.new()
    bmesh.ops.create_cube(bm2, size=1.0)
    bmesh.ops.scale(bm2, vec=Vector((0.040, 0.058, 0.62)), verts=bm2.verts)
    stock = bm_to_object(bm2, "Musket_stock", (mx, my, barrel_z - 0.52))
    stock.rotation_euler = (tilt, 0, 0)
    stock.data.materials.append(make_material("Musket_wood", 0.38, 0.22, 0.10))
    parts.append(stock)

    # Bayonet (tapered spike)
    bm3 = tapered_cone_mesh(r_base=0.007, r_top=0.001, height=0.28, segments=4)
    bayonet = bm_to_object(bm3, "Musket_bayonet", (mx, my, barrel_z + 0.63))
    bayonet.rotation_euler = (tilt, 0, 0)
    bayonet.data.materials.append(MAT_BRASS)
    parts.append(bayonet)

    return parts


# ---------------------------------------------------------------------------
# Assembly
# ---------------------------------------------------------------------------

def assemble_soldier():
    clear_scene()
    build_materials()

    all_parts = []
    all_parts.append(make_torso())
    all_parts.append(make_neck())
    all_parts.append(make_head())
    all_parts.extend(make_shako())
    all_parts.extend(make_arm('L'))
    all_parts.extend(make_arm('R'))
    all_parts.extend(make_leg('L'))
    all_parts.extend(make_leg('R'))
    all_parts.extend(make_crossbelts())
    all_parts.extend(make_musket())

    # Apply rotation transforms before joining
    bpy.ops.object.select_all(action='DESELECT')
    for o in all_parts:
        o.select_set(True)
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)

    soldier = join_objects(all_parts, "BP_BritishInfantry")

    # Origin at ground (feet)
    bpy.context.scene.cursor.location = (0.0, 0.0, 0.0)
    bpy.ops.object.origin_set(type='ORIGIN_CURSOR')

    n_verts = len(soldier.data.vertices)
    n_faces = len(soldier.data.polygons)
    print(f"[BritInf] BP_BritishInfantry — {n_verts} verts, {n_faces} faces")
    return soldier


# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    soldier = assemble_soldier()
    print("Done! File -> Export -> FBX for UE5 import.")
    print("UE5 import settings: Combine Meshes=True, Scale=1.0, Z Up, -Y Forward")
