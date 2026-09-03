/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file GpuLayout.hpp
/// @brief Declaring that a shader-mirroring struct is laid out the way the
/// shader believes.
///
/// A struct shared with a shader is two declarations of one memory layout, in
/// two languages, that no compiler compares. Editing one and not the other does
/// not fail to build and does not fail to run: the shader reads whatever bytes
/// are now at the offset it still believes in, and the picture comes out subtly
/// wrong. That is the worst failure mode a struct can have, and it is entirely
/// preventable — the offsets are compile-time constants on the C++ side.
///
/// The claims are **relative**, and deliberately so. An absolute byte offset per
/// member would work, and would also be a column of numbers nobody can check by
/// reading: whether `shadowCascade` belongs at 288 is a question only a
/// calculator answers, and a wrong one is as silent as the bug it was meant to
/// catch. Saying instead that each member sits immediately after the one before
/// it is a claim a reader can verify at a glance, and it pins the layout just as
/// hard — the first member is anchored at zero and every other follows by
/// induction, so an insertion, a reorder or a retype breaks the chain.
///
/// Use it like this, one line per member, in declaration order:
///
///     struct ThingGpu { glm::vec4 a; glm::vec4 b; };
///     ASSISI_GPU_LAYOUT(ThingGpu);
///     ASSISI_GPU_FIRST_FIELD(ThingGpu, a);
///     ASSISI_GPU_FIELD_AFTER(ThingGpu, b, a);
///     ASSISI_GPU_NO_TAIL_PADDING(ThingGpu, b);
///
/// Macros because `offsetof` is one, and because the diagnostic wants the
/// struct's and the member's names in it — neither of which a function template
/// can recover.

#include <cstddef>
#include <type_traits>

namespace Assisi::Render
{
/// @brief The alignment std430 and std140 both give a vec4, and so the multiple
/// every struct built out of vec4 lanes must be a whole number of.
///
/// It is why these structs are written in vec4/uvec4/mat4 lanes rather than in
/// the scalars they mean: a bare float member would make the C++ struct's size
/// and the shader's stride part company at the first one added.
inline constexpr std::size_t kGpuLaneAlignment = 16;
} // namespace Assisi::Render

/// @brief Assert that @p type is laid out at all, and that its size is a whole
/// number of GPU lanes — which is the array stride a shader's
/// `buffer Foo { Type items[]; }` assumes.
///
/// Standard layout is the precondition for everything else here: without it a
/// compiler may order the members as it likes, and `offsetof` is not required to
/// work. Trivial copyability is because these are uploaded as raw bytes.
#define ASSISI_GPU_LAYOUT(type)                                                                                      \
    static_assert(std::is_standard_layout_v<type>, #type " must be standard-layout to mirror a shader struct.");      \
    static_assert(std::is_trivially_copyable_v<type>,                                                                \
                  #type " must be trivially copyable — it is uploaded to the GPU as bytes.");                         \
    static_assert(sizeof(type) % ::Assisi::Render::kGpuLaneAlignment == 0,                                           \
                  #type " must be a whole number of GPU lanes, or its array stride will not match the shader's.")

/// @brief Assert that @p member is @p type's first, and so begins at the start
/// of the struct.
///
/// The anchor the relative claims below hang from. One per struct.
#define ASSISI_GPU_FIRST_FIELD(type, member)                                                                         \
    static_assert(offsetof(type, member) == 0,                                                                       \
                  #type "::" #member " is no longer first; every offset below it has moved.")

/// @brief Assert that @p member begins immediately after @p previous, with
/// nothing between them.
///
/// One line per member, in declaration order, so the block reads as the layout
/// the shader was written against. A member inserted between the two, or either
/// one retyped, breaks this and every claim after it.
#define ASSISI_GPU_FIELD_AFTER(type, member, previous)                                                               \
    static_assert(offsetof(type, member) == offsetof(type, previous) + sizeof(type::previous),                       \
                  #type "::" #member " no longer sits directly after " #previous "; the shader mirroring it "        \
                  "still reads the old offset.")

/// @brief Assert that @p last is @p type's final member and that the struct ends
/// where it does.
///
/// Closes the chain. Without it a member appended after @p last would leave
/// every claim above intact while changing the stride, which is exactly the
/// silent break this file exists to prevent. A struct with deliberate tail
/// padding names that padding as its last member.
#define ASSISI_GPU_NO_TAIL_PADDING(type, last)                                                                       \
    static_assert(sizeof(type) == offsetof(type, last) + sizeof(type::last),                                         \
                  #type " has bytes past " #last "; either a member was appended or its tail padding changed.")
