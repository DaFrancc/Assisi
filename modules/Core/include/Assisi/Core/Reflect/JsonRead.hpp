/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/JsonRead.hpp
/// @brief Typed, non-throwing field reads for generated deserializers.
///
/// Every reflected component's `addToScene` hook is generated code, and it used
/// to read a field as `j.at("fov").get<float>()`. The `contains()` guard in front
/// of that proves the *key* is there and says nothing about its type, so a file
/// holding `{"fov": "wide"}` reached `get<float>()` and nlohmann threw — the last
/// exception in the engine, and one nobody chose: it was a side effect of which
/// accessor got typed into a codegen template.
///
/// These replace it. Each one answers three questions in the order that matters —
/// is the key present (absent is not an error; see below), is it the right shape,
/// and can it be read — and says so by return value.
///
/// **An absent key is success, and leaves @p out alone.** That is what lets a
/// component gain a field without refusing every level saved before it, and it is
/// the one silent path here on purpose. A key that is *present and wrong* is the
/// opposite: the file says something the engine cannot honour, so it is logged at
/// Error naming the component, the field, what was expected and what was found,
/// and the caller refuses the file.
///
/// Numeric width is deliberately not policed beyond "is a number": JSON has one
/// number type, and a file that says `1` for a float is ordinary. What is caught
/// is a *category* error — a string where a number goes, an object where an array
/// goes, an array of the wrong length.

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace Assisi::Core::Reflect
{

/// @brief Reads a floating-point field. @p component and @p field name the
/// offender in the log; both must outlive the call and are expected to be
/// string literals from generated code.
[[nodiscard]] bool ReadFloat(const nlohmann::json &j, const char *component, const char *field, float &out);
[[nodiscard]] bool ReadDouble(const nlohmann::json &j, const char *component, const char *field, double &out);

/// @brief Reads an integer field. A JSON number with a fractional part is a
/// category error here, not a silent truncation.
[[nodiscard]] bool ReadInt32(const nlohmann::json &j, const char *component, const char *field, int32_t &out);
[[nodiscard]] bool ReadUInt32(const nlohmann::json &j, const char *component, const char *field,
                              uint32_t &out);
[[nodiscard]] bool ReadInt64(const nlohmann::json &j, const char *component, const char *field, int64_t &out);
[[nodiscard]] bool ReadUInt64(const nlohmann::json &j, const char *component, const char *field,
                              uint64_t &out);

[[nodiscard]] bool ReadBool(const nlohmann::json &j, const char *component, const char *field, bool &out);
[[nodiscard]] bool ReadString(const nlohmann::json &j, const char *component, const char *field,
                              std::string &out);

/// @brief Reads a fixed-length array of numbers into @p out — the vec2/3/4, quat
/// and mat4 case.
///
/// The length is checked before a single element is read. Indexing past the end
/// was the other way the old templates threw: a hand-edited `"scale": [1, 1]`
/// walked off `_v[2]`.
///
/// @param count exactly how many elements the field must hold.
/// @param out   an array of at least @p count floats.
[[nodiscard]] bool ReadFloatArray(const nlohmann::json &j, const char *component, const char *field,
                                  std::size_t count, float *out);

/// @brief Whether @p field is present at all, for the templates that hand the
/// value to a helper of their own (AssetId, EntityRef, ComponentMask).
///
/// @param out set to the value when this returns true; untouched otherwise.
[[nodiscard]] bool FindField(const nlohmann::json &j, const char *field, const nlohmann::json *&out);

/// @brief Reports a field whose *value* a caller-supplied reader rejected, in the
/// same shape and at the same severity as the readers above.
///
/// For the cases this file cannot type-check itself — an AssetId that is not a
/// well-formed GUID, an array element of the wrong type — so that every field
/// failure reads the same in a log regardless of which layer noticed it.
void ReportBadField(const char *component, const char *field, std::string_view expected,
                    const nlohmann::json &found);

/// @brief Reads an array field, checking only that it *is* an array.
///
/// For the variable-length cases (a vector of AssetIds or AssetPaths), where the
/// element reads belong to the caller.
[[nodiscard]] bool FindArray(const nlohmann::json &j, const char *component, const char *field,
                             const nlohmann::json *&out);

} // namespace Assisi::Core::Reflect
