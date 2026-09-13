import os

import bpy

# ---- CONFIG ----
lod_configs = [
    # (planar_angle_degrees, collapse_ratio)
    (0,   1.0),   # LOD0 - no reduction (just a clean duplicate)
    (10,  0.9),   # LOD1
    (18,  0.65),  # LOD2
    (25,  0.4),   # LOD3
    (32,  0.18),  # LOD4
]
# Where the per-model directories go, relative to the blend file. Each export
# gets "<name>/<name>.gltf" plus its .bin and textures alongside, so the model
# owns a directory rather than scattering loose files.
export_root = "//"

# Separate rather than .glb: the engine resolves textures from image URIs only,
# and a .glb's images are buffer views with no URI, which imports the material
# factor-only — untextured. Every level still goes in the one .gltf, since the
# chain is read from the "_LOD<n>" node names inside a single file.
export_format = "GLTF_SEPARATE"

# ---- SCRIPT ----
# Every selected mesh, not just the active one: a model is usually several
# objects, and exporting one of them silently drops the rest. The importer
# buckets by level across the whole file, so "Body_LOD1" and "Dial_LOD1" both
# land in level 1 as separate primitives.
source_objs = [obj for obj in bpy.context.selected_objects if obj.type == 'MESH']

if not source_objs:
    raise Exception("No mesh objects selected. Select every part of your model in the viewport or Outliner first, then run this script.")

# The .gltf is named after the active object, which is the one you clicked last.
model_name = (bpy.context.active_object or source_objs[0]).name
print(f"Generating LODs for {len(source_objs)} object(s) -> {model_name}")

lod_objects = []

for i, (angle_deg, collapse_ratio) in enumerate(lod_configs):
    for source_obj in source_objs:
        # Duplicate the source object
        lod_obj = source_obj.copy()
        lod_obj.data = source_obj.data.copy()
        lod_obj.name = f"{source_obj.name}_LOD{i}"
        bpy.context.collection.objects.link(lod_obj)

        if i > 0:  # LOD0 stays untouched
            # Planar decimate pass (cleans flat surfaces)
            planar_mod = lod_obj.modifiers.new(name="Decimate_Planar", type='DECIMATE')
            planar_mod.decimate_type = 'DISSOLVE'
            planar_mod.angle_limit = angle_deg * (3.14159265 / 180)  # degrees to radians

            # Collapse decimate pass (handles curved geo)
            collapse_mod = lod_obj.modifiers.new(name="Decimate_Collapse", type='DECIMATE')
            collapse_mod.decimate_type = 'COLLAPSE'
            collapse_mod.ratio = collapse_ratio

            # Apply both modifiers
            bpy.context.view_layer.objects.active = lod_obj
            for mod in lod_obj.modifiers:
                bpy.ops.object.modifier_apply(modifier=mod.name)

        print(f"{lod_obj.name}: {len(lod_obj.data.polygons)} faces")
        lod_objects.append(lod_obj)

# The source objects are deliberately not selected: they carry no suffix, which
# reads as another LOD0, and their geometry would merge into the real one.
bpy.ops.object.select_all(action='DESELECT')
for lod_obj in lod_objects:
    lod_obj.select_set(True)
bpy.context.view_layer.objects.active = lod_objects[0]

export_dir = bpy.path.abspath(f"{export_root}{model_name}")
os.makedirs(export_dir, exist_ok=True)
filepath = os.path.join(export_dir, f"{model_name}.gltf")
bpy.ops.export_scene.gltf(filepath=filepath, export_format=export_format, use_selection=True)

print(f"LOD generation complete: {filepath}")
