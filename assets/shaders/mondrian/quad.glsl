// What mondrian_quad.vert and mondrian_quad.frag share: the instance buffer,
// the pass constants, and the constants mirroring Assisi::Mondrian's enums.

// The instance buffer as an array of vec4. Each QuadInstance field is read from
// its slot; the values mirror Assisi::Mondrian::InstanceSlot, whose asserts hold
// the C++ struct to the same slots.
const uint kSlotRect         = 0u;
const uint kSlotUv           = 1u;
const uint kSlotClip         = 2u;
const uint kSlotColor        = 3u;
const uint kSlotBorderColor  = 4u;
const uint kSlotCornerRadius = 5u;
const uint kSlotScalars      = 6u; // borderWidth, cornerStyles, kind, transformIndex
const uint kSlotsPerInstance = 7u;

layout(std430, set = 0, binding = 0) readonly buffer Instances
{
    vec4 instanceData[];
};

vec4 InstanceSlot(uint instance, uint slot)
{
    return instanceData[instance * kSlotsPerInstance + slot];
}

// Mirrors Assisi::Mondrian::QuadKind.
const uint kKindSolid     = 0u;
const uint kKindGlyph     = 1u;
const uint kKindImage     = 2u;
const uint kKindNineSlice = 3u;

// Mirrors Assisi::Mondrian::CornerStyle, packed per Assisi::Mondrian::Corner
// (top-left, top-right, bottom-right, bottom-left) at kCornerStyleBits each.
const uint kCornerSquare    = 0u;
const uint kCornerRounded   = 1u;
const uint kCornerCut       = 2u;
const uint kCornerStyleBits = 2u;
const uint kCornerStyleMask = 3u;

layout(push_constant) uniform PassConstants
{
    vec2 viewport;   // target width and height in pixels
    uint encodeSrgb; // nonzero when the target encodes sRGB on write
} pass;
