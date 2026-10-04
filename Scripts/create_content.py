#
# Copyright (c) 2026, Daily
#
# SPDX-License-Identifier: BSD-2-Clause
#

"""Creates the game's materials, in /Game/Room.

- M_Flat, everything in the house, and the characters: a flat color, with no
  texture, matte or polished (Roughness), and lamps glow with Emissive times
  EmissiveStrength. A character glows at its edges with RimColor, as much as
  Rim says, e.g. when it can hear the player: that glow looks the same however
  bright the room is.
- M_Ring, the waveform around the player as they speak: unlit and additive,
  in its vertices' color, and brightest along the middle of its ribbon, which
  its texture coordinates go across (V from 0 to 1). It looks the same however
  bright the room is, scaled by Glow.

setup.ps1 runs it in the editor, e.g.:

    UnrealEditor-Cmd PipecatRoom.uproject -run=pythonscript \\
        -script=Scripts/create_content.py
"""

import unreal

FLAT = "/Game/Room/M_Flat"
RING = "/Game/Room/M_Ring"

assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
editing = unreal.MaterialEditingLibrary

for path in (FLAT, RING):
    if assets.does_asset_exist(path):
        assets.delete_asset(path)


def new_material(path):
    folder, name = path.rsplit("/", 1)
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    return tools.create_asset(name, folder, unreal.Material, unreal.MaterialFactoryNew())


def scalar(material, name, value, y):
    node = editing.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -700, y)
    node.set_editor_property("parameter_name", name)
    node.set_editor_property("default_value", value)
    return node


def color(material, name, value, y):
    node = editing.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -700, y)
    node.set_editor_property("parameter_name", name)
    node.set_editor_property("default_value", unreal.LinearColor(value[0], value[1], value[2], 1.0))
    return node


def node(material, kind, x, y):
    return editing.create_material_expression(material, kind, x, y)


def multiply(material, a, b, x, y):
    result = node(material, unreal.MaterialExpressionMultiply, x, y)
    connect(a, result, "A")
    connect(b, result, "B")
    return result


def add(material, a, b, x, y):
    result = node(material, unreal.MaterialExpressionAdd, x, y)
    connect(a, result, "A")
    connect(b, result, "B")
    return result


def connect(source, target, input_name):
    """Connects `source`, a node or a (node, output) pair, to `target`'s input."""
    source, output = source if isinstance(source, tuple) else (source, "")
    editing.connect_material_expressions(source, output, target, input_name)


def output(source, prop):
    source, name = source if isinstance(source, tuple) else (source, "")
    editing.connect_material_property(source, name, prop)


#
# M_Flat
#

flat = new_material(FLAT)
for usage in ("used_with_skeletal_mesh", "used_with_instanced_static_meshes"):
    flat.set_editor_property(usage, True)

output(color(flat, "Color", (0.75, 0.73, 0.7), 0), unreal.MaterialProperty.MP_BASE_COLOR)
output(scalar(flat, "Roughness", 0.65, 150), unreal.MaterialProperty.MP_ROUGHNESS)
output(scalar(flat, "Specular", 0.5, 250), unreal.MaterialProperty.MP_SPECULAR)
output(scalar(flat, "Metallic", 0.0, 350), unreal.MaterialProperty.MP_METALLIC)

# A lamp's glow, and a character's, at its edges.
glow = multiply(flat, color(flat, "Emissive", (0.0, 0.0, 0.0), 500), scalar(flat, "EmissiveStrength", 0.0, 600), -450, 550)
edges = node(flat, unreal.MaterialExpressionFresnel, -700, 800)
edges.set_editor_property("exponent", 3.0)
edges.set_editor_property("base_reflect_fraction", 0.04)
rim = multiply(flat, color(flat, "RimColor", (1.0, 1.0, 1.0), 700), scalar(flat, "Rim", 0.0, 750), -450, 720)
rim = multiply(flat, rim, edges, -300, 760)
rim = multiply(flat, rim, node(flat, unreal.MaterialExpressionEyeAdaptationInverse, -450, 900), -150, 800)
output(add(flat, glow, rim, 0, 650), unreal.MaterialProperty.MP_EMISSIVE_COLOR)

editing.recompile_material(flat)
assets.save_asset(FLAT)

#
# M_Ring
#

ring = new_material(RING)
ring.set_editor_property("blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
ring.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
ring.set_editor_property("two_sided", True)
# Moving every frame, it has no history for the upscaler to rely on.
for prop in ("enable_responsive_aa", "output_translucent_velocity"):
    try:
        ring.set_editor_property(prop, True)
    except Exception as error:  # noqa: BLE001 - only a refinement
        unreal.log_warning(f"M_Ring: no {prop}: {error}")

# Brightest along the middle of the ribbon, fading to nothing at its edges.
RING_CODE = """
float Across = saturate(1.0 - abs(UV.y * 2.0 - 1.0));
return Color * Alpha * Glow * Across * Across * Exposure;
"""
vertex = node(ring, unreal.MaterialExpressionVertexColor, -900, 0)
code = node(ring, unreal.MaterialExpressionCustom, -300, 100)
code.set_editor_property("code", RING_CODE)
code.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
inputs = {
    "Color": vertex,  # its first output: RGB
    "Alpha": (vertex, "A"),
    "UV": node(ring, unreal.MaterialExpressionTextureCoordinate, -900, 300),
    "Glow": scalar(ring, "Glow", 4.0, 400),
    "Exposure": node(ring, unreal.MaterialExpressionEyeAdaptationInverse, -900, 500),
}
custom_inputs = []
for name in inputs:
    custom_input = unreal.CustomInput()
    custom_input.set_editor_property("input_name", name)
    custom_inputs.append(custom_input)
code.set_editor_property("inputs", custom_inputs)
for name, source in inputs.items():
    connect(source, code, name)
output(code, unreal.MaterialProperty.MP_EMISSIVE_COLOR)

editing.recompile_material(ring)
assets.save_asset(RING)

unreal.log("Created /Game/Room/M_Flat and /Game/Room/M_Ring")
