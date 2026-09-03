/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestFieldBounds.cpp
/// @brief A bound that names a sibling field is that field's value now, not a
/// number decided when the component was written.
///
/// The failure this guards against is silent in both directions. FieldMeta keeps
/// `minValue` and `minField` side by side and only one of them is ever set, so a
/// reader that takes the literal without checking the name clamps to zero — and
/// zero is a legal bound, so nothing about the result says it was invented. The
/// other direction is a name that resolves to nothing, where inventing a bound is
/// the same lie; the bound has to be dropped instead.
///
/// The bounds are built by hand here rather than through reflectgen, because the
/// generator's job is covered by its own suite and what needs proving here is
/// what the resolver does with metadata once it exists — including shapes the
/// generator refuses to emit, which are exactly the ones a wrong caller produces.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <Assisi/Core/Reflect/FieldMeta.hpp>

using Assisi::Core::Reflect::FieldBounds;
using Assisi::Core::Reflect::FieldMeta;
using Assisi::Core::Reflect::FieldType;
using Assisi::Core::Reflect::ReadNumericField;
using Assisi::Core::Reflect::ResolveFieldBounds;
using Assisi::Core::Reflect::SettleDependentBounds;
using Assisi::Core::Reflect::WriteNumericField;

namespace
{

/// Stands in for a spot light: a cone whose core may not open past its cutoff.
struct Cone
{
    float inner = 15.f;
    float outer = 30.f;
    std::int32_t count = 7;
};

/// `inner` capped by `outer`, `outer` free — the shape the light components use.
std::vector<FieldMeta> ConeFields()
{
    return {
        FieldMeta{.name = "inner", .type = FieldType::Float, .offset = offsetof(Cone, inner),
                  .hasMin = true, .hasMax = true, .minValue = 0.f, .maxField = "outer"},
        FieldMeta{.name = "outer", .type = FieldType::Float, .offset = offsetof(Cone, outer),
                  .hasMin = true, .hasMax = true, .minValue = 0.f, .maxValue = 89.f},
        FieldMeta{.name = "count", .type = FieldType::Int32, .offset = offsetof(Cone, count)},
    };
}

} // namespace

TEST_CASE("ResolveFieldBounds: a named bound is the sibling's current value")
{
    const std::vector<FieldMeta> fields = ConeFields();
    Cone cone;

    FieldBounds bounds = ResolveFieldBounds(fields[0], fields, &cone);
    CHECK(bounds.hasMax);
    CHECK(bounds.maxValue == doctest::Approx(30.0));

    // The point of naming a field rather than writing a number: move the outer
    // cone and the cap on the inner one moves with it, with nothing to keep in
    // step and no second place the limit is written down.
    cone.outer = 45.f;
    bounds     = ResolveFieldBounds(fields[0], fields, &cone);
    CHECK(bounds.maxValue == doctest::Approx(45.0));

    // The literal half of the same field is untouched by any of that.
    CHECK(bounds.hasMin);
    CHECK(bounds.minValue == doctest::Approx(0.0));
}

TEST_CASE("ResolveFieldBounds: a literal bound is the number as written")
{
    const std::vector<FieldMeta> fields = ConeFields();
    Cone cone;

    const FieldBounds bounds = ResolveFieldBounds(fields[1], fields, &cone);
    CHECK(bounds.hasMin);
    CHECK(bounds.hasMax);
    CHECK(bounds.minValue == doctest::Approx(0.0));
    CHECK(bounds.maxValue == doctest::Approx(89.0));
}

TEST_CASE("ResolveFieldBounds: a field with no bounds reports none")
{
    const std::vector<FieldMeta> fields = ConeFields();
    Cone cone;

    const FieldBounds bounds = ResolveFieldBounds(fields[2], fields, &cone);
    CHECK_FALSE(bounds.hasMin);
    CHECK_FALSE(bounds.hasMax);
}

TEST_CASE("ResolveFieldBounds: a name that resolves to nothing drops the bound")
{
    // Zero is a legal bound, so a resolver that fell back to minValue here would
    // clamp the field to zero and look like the author had asked for it.
    std::vector<FieldMeta> fields = ConeFields();
    fields[0].maxField = "notAField";

    Cone cone;
    const FieldBounds bounds = ResolveFieldBounds(fields[0], fields, &cone);
    CHECK_FALSE(bounds.hasMax);
    // The other side still holds: one unusable bound does not discard the field's
    // whole range.
    CHECK(bounds.hasMin);
    CHECK(bounds.minValue == doctest::Approx(0.0));
}

TEST_CASE("ResolveFieldBounds: a name pointing at a field with no number drops the bound")
{
    std::vector<FieldMeta> fields = ConeFields();
    fields.push_back(FieldMeta{.name = "label", .type = FieldType::String, .offset = 0});
    fields[0].maxField = "label";

    Cone cone;
    CHECK_FALSE(ResolveFieldBounds(fields[0], fields, &cone).hasMax);
}

TEST_CASE("ResolveFieldBounds: without an object a named bound has nothing to read")
{
    const std::vector<FieldMeta> fields = ConeFields();

    const FieldBounds bounds = ResolveFieldBounds(fields[0], fields, nullptr);
    CHECK_FALSE(bounds.hasMax);
    // A literal needs no object, so it survives.
    CHECK(bounds.hasMin);
}

TEST_CASE("ResolveFieldBounds: two fields may bound each other")
{
    // Resolution is one step and never consults the named field's own bounds, so
    // a mutual pair terminates and means what it reads as: neither may cross the
    // other.
    std::vector<FieldMeta> fields = ConeFields();
    fields[1].minValue = 0.f;
    fields[1].minField = "inner";

    Cone cone;
    CHECK(ResolveFieldBounds(fields[0], fields, &cone).maxValue == doctest::Approx(30.0));
    CHECK(ResolveFieldBounds(fields[1], fields, &cone).minValue == doctest::Approx(15.0));
}

TEST_CASE("SettleDependentBounds: narrowing the cap drags the capped field down")
{
    // The asymmetry this exists for. `outer` has no upper bound of its own, so
    // dragging it down past `inner` is a perfectly legal edit to *that* field —
    // and nothing on `inner`'s own widget runs to notice, because the drag is
    // happening somewhere else.
    const std::vector<FieldMeta> fields = ConeFields();
    Cone cone;
    cone.outer = 10.f;

    CHECK(SettleDependentBounds(fields, &cone));
    CHECK(cone.inner == doctest::Approx(10.f));
    // The field that was edited is not itself moved back.
    CHECK(cone.outer == doctest::Approx(10.f));
}

TEST_CASE("SettleDependentBounds: a pair already in range is left alone")
{
    const std::vector<FieldMeta> fields = ConeFields();
    Cone cone;

    CHECK_FALSE(SettleDependentBounds(fields, &cone));
    CHECK(cone.inner == doctest::Approx(15.f));
    CHECK(cone.outer == doctest::Approx(30.f));
}

TEST_CASE("SettleDependentBounds: a literal-bounded field is not rewritten")
{
    // `outer`'s bounds are both literals, so nothing an author does to another
    // field can invalidate them — a value outside came from a hand-edited file,
    // and silently correcting it would hide that.
    const std::vector<FieldMeta> fields = ConeFields();
    Cone cone;
    cone.outer = 200.f; // past the literal max of 89

    SettleDependentBounds(fields, &cone);
    CHECK(cone.outer == doctest::Approx(200.f));
}

TEST_CASE("SettleDependentBounds: a chain settles in one call")
{
    struct Chain
    {
        float a = 100.f;
        float b = 50.f;
        float c = 25.f;
    };
    // Listed against the direction of the dependency — c follows b follows a — so
    // a single pass cannot cascade: it reaches `c` while `b` is still stale, and
    // only fixes `b` afterwards. Declaration order is the author's choice and says
    // nothing about which field caps which, so the settle cannot rely on it.
    const std::vector<FieldMeta> fields{
        FieldMeta{.name = "c", .type = FieldType::Float, .offset = offsetof(Chain, c),
                  .hasMax = true, .maxField = "b"},
        FieldMeta{.name = "b", .type = FieldType::Float, .offset = offsetof(Chain, b),
                  .hasMax = true, .maxField = "a"},
        FieldMeta{.name = "a", .type = FieldType::Float, .offset = offsetof(Chain, a)},
    };

    // `a` drops below both.
    Chain chain;
    chain.a = 10.f;

    CHECK(SettleDependentBounds(fields, &chain));
    CHECK(chain.b == doctest::Approx(10.f));
    CHECK(chain.c == doctest::Approx(10.f));
}

TEST_CASE("SettleDependentBounds: a mutually bounded pair terminates")
{
    // Both directions named. Clamping only ever moves a field *toward* the other,
    // so this converges rather than ping-ponging — and the pass cap holds even if
    // some future metadata does not.
    std::vector<FieldMeta> fields = ConeFields();
    fields[1].minField = "inner";

    Cone cone;
    cone.outer = 5.f;

    CHECK(SettleDependentBounds(fields, &cone));
    CHECK(cone.inner == doctest::Approx(5.f));
    CHECK(cone.outer == doctest::Approx(5.f));
}

TEST_CASE("SettleDependentBounds: a NaN is left where it is")
{
    // Not ordered against either bound, so there is no direction to move it in —
    // and it compares unequal to itself, which is what would otherwise read as
    // "moved" on every pass.
    const std::vector<FieldMeta> fields = ConeFields();
    Cone cone;
    cone.inner = std::numeric_limits<float>::quiet_NaN();

    CHECK_FALSE(SettleDependentBounds(fields, &cone));
    CHECK(std::isnan(cone.inner));
}

TEST_CASE("WriteNumericField: writes at the field's own width, and refuses the rest")
{
    const std::vector<FieldMeta> fields = ConeFields();
    Cone cone;

    REQUIRE(WriteNumericField(fields[0], &cone, 12.5));
    CHECK(cone.inner == doctest::Approx(12.5f));

    // Narrowed to the field's type rather than written as a double over it, which
    // would take the neighbouring bytes with it.
    REQUIRE(WriteNumericField(fields[2], &cone, 42.9));
    CHECK(cone.count == 42);
    CHECK(cone.outer == doctest::Approx(30.f));

    const FieldMeta text{.name = "label", .type = FieldType::String, .offset = 0};
    CHECK_FALSE(WriteNumericField(text, &cone, 1.0));
}

TEST_CASE("ReadNumericField: every numeric width reads, and nothing else does")
{
    const std::vector<FieldMeta> fields = ConeFields();
    Cone cone;

    double value = -1.0;
    REQUIRE(ReadNumericField(fields[0], &cone, value));
    CHECK(value == doctest::Approx(15.0));

    // An integer field reads at its own width, not reinterpreted as a float.
    REQUIRE(ReadNumericField(fields[2], &cone, value));
    CHECK(value == doctest::Approx(7.0));

    // A type with no single number leaves the output alone rather than reading
    // whatever bytes are at the offset.
    const FieldMeta text{.name = "label", .type = FieldType::String, .offset = 0};
    value = -1.0;
    CHECK_FALSE(ReadNumericField(text, &cone, value));
    CHECK(value == doctest::Approx(-1.0));
}
