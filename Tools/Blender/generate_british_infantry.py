"""
Poor Man's Total War - British Line Infantry Generator
Run in Blender (Scripting tab) to generate a Napoleonic-era British infantry soldier.

Usage:
  1. Open Blender -> Scripting tab
  2. Paste this script or open the file
  3. Press "Run Script"
  4. File -> Export -> FBX -> import into UE5

The model targets ~800-1200 polygons suitable for instanced rendering
of thousands of units via UE5 Mass Entity / HISM.
"""

import bpy
import bmesh
from mathutils import Vector, Matrix
import math


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def clear_scene():
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    for col in list(bpy.data.collections):
        bpy.data.collections.remove(col)


def new_collection(name):
    col = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(col)
    return col


def add_to_collection(obj, col):
    for c in obj.users_collection:
        c.objects.unlink(obj)
    col.objects.link(obj)


def make_material(name, r, g, b, a=1.0):
    mat = bpy.data.materials.new(name=name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (r, g, b, a)
    bsdf.inputs["Roughness"].default_value = 0.8
    bsdf.inputs["Metallic"].default_value = 0.0
    return mat


def extrude_face_scaled(bm, face, offset, scale_x=1.0, scale_y=1.0):
    """Extrude a single bmesh face along its normal, scale the new face."""
    result = bmesh.ops.extrude_face_region(bm, geom=[face])
    new_verts = [v for v in result["geom"] if isinstance(v, bmesh.types.BMVert)]
    new_face = [f for f in result["geom"] if isinstance(f, bmesh.types.BMFace)][0]
    center = new_face.calc_center_median()
    normal = new_face.normal
    for v in new_verts:
        v.co += normal * offset
    bmesh.ops.scale(bm, vec=Vector((scale_x, scale_y, 1.0)),
                    space=Matrix.Translation(center),
                    verts=new_verts)
    return new_face


def cylinder_mesh(radius, height, segments=8, cap_ends=True):
    """Return a bmesh cylinder."""
    bm = bmesh.new()
    bmesh.ops.create_cone(
        bm,
        cap_ends=cap_ends,
        cap_tris=True,
        segments=segments,
        radius1=radius,
        radius2=radius,
        depth=height,
    )
    return bm


def box_mesh(sx, sy, sz):
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector((sx, sy, sz)), verts=bm.verts)
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
    """Join a list of objects into one and rename."""
    bpy.ops.object.select_all(action='DESELECT')
    for o in objects:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]
    bpy.ops.object.join()
    bpy.context.active_object.name = name
    return bpy.context.active_object


# ---------------------------------------------------------------------------
# Materials (Napoleonic British palette)
# ---------------------------------------------------------------------------
MAT_COAT      = None   # scarlet red coat
MAT_TROUSERS  = None   # grey/white trousers
MAT_SHAKO     = None   # black shako
MAT_SKIN      = None   # face/hands
MAT_BRASS     = None   # buttons, plate
MAT_CROSSBELT = None   # white leather cross-belts
MAT_BOOT      = None   # black boots/gaiters


def build_materials():
    global MAT_COAT, MAT_TROUSERS, MAT_SHAKO, MAT_SKIN, MAT_BRASS, MAT_CROSSBELT, MAT_BOOT
    MAT_COAT      = make_material("BritInf_Coat",      0.60, 0.05, 0.05)
    MAT_TROUSERS  = make_material("BritInf_Trousers",  0.85, 0.83, 0.78)
    MAT_SHAKO     = make_material("BritInf_Shako",     0.05, 0.05, 0.05)
    MAT_SKIN      = make_material("BritInf_Skin",      0.80, 0.62, 0.48)
    MAT_BRASS     = make_material("BritInf_Brass",     0.72, 0.55, 0.10)
    MAT_CROSSBELT = make_material("BritInf_Crossbelt", 0.92, 0.90, 0.85)
    MAT_BOOT      = make_material("BritInf_Boot",      0.06, 0.06, 0.06)


# ---------------------------------------------------------------------------
# Body parts (all sizes in Blender units ≈ meters, humanoid ~1.80m)
# ---------------------------------------------------------------------------

# Coordinate convention: Z up, model faces +Y

TORSO_W  = 0.30   # half-width
TORSO_D  = 0.14   # half-depth
TORSO_H  = 0.46   # total height
TORSO_Z  = 0.86   # centre Z

HEAD_R   = 0.12
HEAD_Z   = TORSO_Z + TORSO_H / 2 + HEAD_R * 0.9

NECK_R   = 0.055
NECK_H   = 0.10

UPPER_ARM_R = 0.055
UPPER_ARM_H = 0.27
LOWER_ARM_R = 0.045
LOWER_ARM_H = 0.24

HAND_SX  = 0.07
HAND_SY  = 0.04
HAND_SZ  = 0.09

UPPER_LEG_R = 0.075
UPPER_LEG_H = 0.38
LOWER_LEG_R = 0.060
LOWER_LEG_H = 0.38

FOOT_SX  = 0.07
FOOT_SY  = 0.15
FOOT_SZ  = 0.06

HIP_OFFSET_X = 0.095  # lateral hip separation


def make_torso():
    bm = box_mesh(TORSO_W * 2, TORSO_D * 2, TORSO_H)
    obj = bm_to_object(bm, "Torso", (0, 0, TORSO_Z))
    obj.data.materials.append(MAT_COAT)
    return obj


def make_head():
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=8, v_segments=6, radius=HEAD_R)
    # flatten slightly front-back
    bmesh.ops.scale(bm, vec=Vector((1.0, 0.80, 1.0)), verts=bm.verts)
    obj = bm_to_object(bm, "Head", (0, 0, HEAD_Z))
    obj.data.materials.append(MAT_SKIN)
    return obj


def make_neck():
    bm = cylinder_mesh(NECK_R, NECK_H, segments=6)
    obj = bm_to_object(bm, "Neck", (0, 0, TORSO_Z + TORSO_H / 2 + NECK_H / 2 - 0.01))
    obj.data.materials.append(MAT_SKIN)
    return obj


def make_shako():
    """Cylindrical Belgic shako with a small brim and flat top."""
    parts = []

    # Main cylinder (body of shako)
    bm = cylinder_mesh(0.105, 0.165, segments=10)
    cap = bm_to_object(bm, "Shako_body", (0, 0, HEAD_Z + HEAD_R * 0.80 + 0.165 / 2))
    cap.data.materials.append(MAT_SHAKO)
    parts.append(cap)

    # Brim (torus-like ring - approximate with scaled cylinder)
    bm2 = cylinder_mesh(0.140, 0.025, segments=10, cap_ends=True)
    brim = bm_to_object(bm2, "Shako_brim",
                        (0, 0, HEAD_Z + HEAD_R * 0.80 + 0.012))
    brim.data.materials.append(MAT_SHAKO)
    parts.append(brim)

    # Brass plate on front
    bm3 = box_mesh(0.06, 0.01, 0.06)
    plate = bm_to_object(bm3, "Shako_plate",
                         (0, 0.135, HEAD_Z + HEAD_R * 0.80 + 0.07))
    plate.data.materials.append(MAT_BRASS)
    parts.append(plate)

    return parts


def make_arm(side):
    """side: 'L' or 'R'. Returns list of part objects."""
    sign = 1 if side == 'L' else -1
    shoulder_x = sign * (TORSO_W + UPPER_ARM_R * 0.6)
    parts = []

    # Upper arm — angled slightly down
    bm = cylinder_mesh(UPPER_ARM_R, UPPER_ARM_H, segments=6)
    uarm = bm_to_object(bm, f"UpperArm_{side}",
                        (shoulder_x, 0, TORSO_Z + TORSO_H / 2 - UPPER_ARM_H / 2 - 0.04))
    uarm.data.materials.append(MAT_COAT)
    parts.append(uarm)

    elbow_z = TORSO_Z + TORSO_H / 2 - UPPER_ARM_H - 0.04

    # Lower arm
    bm2 = cylinder_mesh(LOWER_ARM_R, LOWER_ARM_H, segments=6)
    larm = bm_to_object(bm2, f"LowerArm_{side}",
                        (shoulder_x, 0, elbow_z - LOWER_ARM_H / 2))
    larm.data.materials.append(MAT_COAT)
    parts.append(larm)

    # Hand
    wrist_z = elbow_z - LOWER_ARM_H
    bm3 = box_mesh(HAND_SX, HAND_SY, HAND_SZ)
    hand = bm_to_object(bm3, f"Hand_{side}",
                        (shoulder_x, 0, wrist_z - HAND_SZ / 2))
    hand.data.materials.append(MAT_SKIN)
    parts.append(hand)

    return parts


def make_leg(side):
    sign = 1 if side == 'L' else -1
    hip_x = sign * HIP_OFFSET_X
    parts = []

    # Upper leg
    bm = cylinder_mesh(UPPER_LEG_R, UPPER_LEG_H, segments=7)
    uleg = bm_to_object(bm, f"UpperLeg_{side}",
                        (hip_x, 0, TORSO_Z - TORSO_H / 2 - UPPER_LEG_H / 2))
    uleg.data.materials.append(MAT_TROUSERS)
    parts.append(uleg)

    knee_z = TORSO_Z - TORSO_H / 2 - UPPER_LEG_H

    # Lower leg (gaiter / boot)
    bm2 = cylinder_mesh(LOWER_LEG_R, LOWER_LEG_H, segments=7)
    lleg = bm_to_object(bm2, f"LowerLeg_{side}",
                        (hip_x, 0, knee_z - LOWER_LEG_H / 2))
    lleg.data.materials.append(MAT_BOOT)
    parts.append(lleg)

    # Foot
    foot_z = knee_z - LOWER_LEG_H - FOOT_SZ / 2
    bm3 = box_mesh(FOOT_SX, FOOT_SY, FOOT_SZ)
    foot = bm_to_object(bm3, f"Foot_{side}", (hip_x, FOOT_SY * 0.3, foot_z))
    foot.data.materials.append(MAT_BOOT)
    parts.append(foot)

    return parts


def make_crossbelts():
    """Two thin white diagonal straps across the chest."""
    parts = []
    # Right-shoulder-to-left-hip belt
    bm = box_mesh(0.025, 0.015, 0.54)
    belt1 = bm_to_object(bm, "Belt_RS_LH", (0.04, TORSO_D + 0.005, TORSO_Z))
    belt1.rotation_euler = (0, math.radians(15), math.radians(-22))
    belt1.data.materials.append(MAT_CROSSBELT)
    parts.append(belt1)

    # Left-shoulder-to-right-hip belt
    bm2 = box_mesh(0.025, 0.015, 0.54)
    belt2 = bm_to_object(bm2, "Belt_LS_RH", (-0.04, TORSO_D + 0.005, TORSO_Z))
    belt2.rotation_euler = (0, math.radians(-15), math.radians(22))
    belt2.data.materials.append(MAT_CROSSBELT)
    parts.append(belt2)

    return parts


def make_musket():
    """Simple Brown Bess musket — barrel + stock."""
    parts = []
    barrel_z = TORSO_Z - 0.05
    hand_x = -1 * (TORSO_W + UPPER_ARM_R * 0.6)  # right hand side

    # Barrel
    bm = cylinder_mesh(0.018, 1.20, segments=5)
    barrel = bm_to_object(bm, "Musket_barrel",
                          (hand_x - 0.04, 0.04, barrel_z))
    barrel.rotation_euler = (math.radians(10), 0, 0)
    barrel.data.materials.append(MAT_SHAKO)  # dark metal
    parts.append(barrel)

    # Stock
    bm2 = box_mesh(0.04, 0.06, 0.65)
    stock = bm_to_object(bm2, "Musket_stock",
                         (hand_x - 0.04, 0.04, barrel_z - 0.55))
    stock.rotation_euler = (math.radians(10), 0, 0)
    stock.data.materials.append(make_material("Musket_wood", 0.38, 0.22, 0.10))
    parts.append(stock)

    # Bayonet
    bm3 = cylinder_mesh(0.008, 0.30, segments=4)
    bayonet = bm_to_object(bm3, "Musket_bayonet",
                           (hand_x - 0.04, 0.04, barrel_z + 0.65))
    bayonet.rotation_euler = (math.radians(10), 0, 0)
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

    # Apply transforms so join works cleanly
    bpy.ops.object.select_all(action='DESELECT')
    for o in all_parts:
        o.select_set(True)
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)

    soldier = join_objects(all_parts, "BP_BritishInfantry")

    # Recenter origin to feet
    bpy.ops.object.origin_set(type='ORIGIN_CURSOR')
    bpy.context.scene.cursor.location = (0.0, 0.0, 0.0)
    bpy.ops.object.origin_set(type='ORIGIN_CURSOR')

    print(f"[BritInf] Soldier created: {len(soldier.data.vertices)} verts, "
          f"{len(soldier.data.polygons)} faces")
    return soldier


# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    soldier = assemble_soldier()
    print("Done! Select the object and File -> Export -> FBX for UE5 import.")
    print("Tip: in UE5 FBX import, enable 'Combine Meshes' and set scale to 1.0")
