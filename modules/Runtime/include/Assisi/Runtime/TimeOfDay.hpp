/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TimeOfDay.hpp
/// @brief What time it is, where on the planet, and what the planet is.
///
/// Three components, all on one entity by convention and none of them required to
/// be. TimeOfDay is the clock; Sun says the aim comes from that clock and where
/// the sky is being watched from; Moon adds a second body. Where a body actually
/// ends up is Render::Celestial's business — this is the state and its advance.
///
/// **Presence is the disclosure.** A level with no Moon has no moon fields to
/// read, hide or grey: they are absent, and nothing computes them. That is why
/// there is no advanced-mode toggle here and should not be one — a toggle would
/// store an editor preference in scene data, which describes the world rather
/// than the machine rendering it.

#include <cstdint>

#include <Assisi/Math/Color.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Prelude.hpp>
#include <Assisi/Render/Celestial.hpp>

namespace Assisi::Runtime
{

/// @name What a day and a year may be worth
///
/// A period rather than a rate at both scales, and for the same reason: a rate
/// approaching zero is indistinguishable from the clock being paused, where a
/// period approaching zero is a strobe that a floor keeps out. Freezing is
/// @ref TimeOfDay::paused and @ref TimeOfDay::seasonsPaused, never a zero.
/// @{

/// Below a second a day is a strobe, not a time of day.
inline constexpr float kMinDayLengthSeconds = 1.f;
/// A week. Slower than this is not a slow day, it is a paused one.
inline constexpr float kMaxDayLengthSeconds = 604800.f;

/// A year inside a single day still has four seasons in it, which is a legitimate
/// thing to want; below one there is no day for a season to fall on.
inline constexpr float kMinYearLengthDays = 1.f;
inline constexpr float kMaxYearLengthDays = 100000.f;
/// @}

/// @brief The scene's clock: a daily one and an annual one, of identical shape.
///
/// One per scene. The resolver takes the first it finds.
///
/// **Integrated, not evaluated from a tick.** The hour and the day of the year are
/// state — jumps set them, and the rates change underneath them — and integrating
/// is what makes a mid-session rate change continuous instead of a jump to
/// wherever the new rate says the tick lands.
ACOMP()
struct TimeOfDay
{
    /// @name The daily clock
    /// @{

    /// Apparent solar time: the sun crosses the meridian at 12:00 every day and
    /// there is no equation of time.
    ///
    /// **A double, and it has to be.** At a twenty-four hour day and sixty hertz
    /// the step is 4.6e-6 hours, and a float's spacing in the evening hours is
    /// 1.9e-6 — so the step would round to two of them and the clock would run
    /// about a fifth slow through the second half of every day.
    AFIELD(min = 0.0, max = 24.0) double hour = 12.0;

    /// Days since the level began. The moon's phase counts these, so it survives
    /// a save rather than restarting.
    AFIELD(min = 0) int32_t day = 0;

    AFIELD(min = 1.0, max = 604800.0) float dayLengthSeconds = 600.f;
    AFIELD() bool paused = false;
    /// @}

    /// @name The annual clock, the same shape
    /// @{

    /// Zero is the March equinox, which is where the year is measured from.
    ///
    /// **Independent state, not derived from @ref day.** That independence is
    /// exactly what buys a frozen season under a running day — noon holding still
    /// while the shadow swings through the year, or the reverse — and it costs one
    /// field in the level file.
    ///
    /// A double for a worse reason than the hour: at a twenty-four hour day its
    /// step is 1.9e-7 days, which is a fortieth of a float's spacing at 8. The
    /// year would never advance at all.
    AFIELD(min = 0.0) double dayOfYear = 0.0;

    /// Simulated days per year. Twelve, not three hundred and sixty-five, because
    /// the two rates multiply: at the default ten-minute day a real-length year is
    /// sixty hours of play and nobody ever sees a season. Twelve makes a season
    /// three days — half an hour at the default rate — and a day-to-day
    /// declination step of at most twelve degrees, enough to notice and not enough
    /// to jar.
    ///
    /// The moon's month is NOT tied to this. A month longer than the year is odd
    /// but finite, and at the defaults the seasons cycle about two and a half
    /// times per lunation.
    AFIELD(min = 1.0, max = 100000.0) float yearLengthDays = 12.f;
    AFIELD() bool seasonsPaused = false;
    /// @}

    /// How many times the clock has been cut rather than run. Consumers that hold
    /// history — the shadow cadence, a probe grid — watch this to tell a cut from
    /// a fast-forward.
    ///
    /// **Deliberately not reflected.** It is a counter with no meaning outside the
    /// session that produced it, and a reflected one would rewrite the level file
    /// every time anybody scrubbed the clock.
    uint32_t jumpSerial = 0;
};

/// @brief The sun's aim comes from the clock, and this is where the sky is being
/// watched from.
///
/// On the entity carrying the DirectionalLight, whose `direction` then stops being
/// read. Without this component that light is aimed as authored, which is what a
/// light placed by hand for one shot wants.
ACOMP()
struct Sun
{
    /// North positive. The pole is a real place and is reachable: the direction
    /// formula is a circle about the celestial pole with no tangent in it, so
    /// there is nothing there to avoid.
    AFIELD(min = -90.0, max = 90.0) float latitudeDegrees = 45.f;

    /// The angle between the planet's spin axis and its orbital plane.
    ///
    /// **Zero is the no-seasons limit**, not a disabled feature: the declination
    /// is then identically zero, every day is an equinox, day and night are twelve
    /// hours at every latitude and the sun rises due east every morning.
    ///
    /// **Seasonal colour is not a field here and must never become one.** Winter
    /// light is redder because the sun is LOWER — the beam crosses more air — and
    /// SunlightTransmittance already computes exactly that from the elevation this
    /// tilt produces. A knob for it would multiply the same reddening in twice.
    AFIELD(min = 0.0, max = 90.0) float axialTiltDegrees = 23.44f;

    /// Which way the level's east faces, as a turn about world up. Without it,
    /// moving where the sun rises means turning the level, which a level with a
    /// fixed layout cannot do.
    AFIELD(min = -180.0, max = 180.0) float bearingDegrees = 0.f;
};

/// @brief A moon: drawn in the sky whenever it is up, and lighting the world at
/// night.
///
/// Requires a @ref Sun on the same entity, because its orbit is defined against
/// that sun's ecliptic. Without one it is ignored.
///
/// **The image turns against the horizon as the moon crosses the sky.** That is
/// parallactic rotation and the real moon shows it; the disk's up is the ecliptic
/// pole rather than the zenith, which is what makes it happen and what stops the
/// picture flipping over as the moon transits overhead.
ACOMP()
struct Moon
{
    /// The moon's light on the scale the sun's is, so 1 would be a second sun.
    /// Real moonlight is nearer 2.5e-6, which is unplayably dark; this is a
    /// compromise and is meant to be one.
    AFIELD(min = 0) float intensity = 0.02f;
    AFIELD() Assisi::Math::Color3 color{0.75f, 0.85f, 1.0f};

    AFIELD(min = 0.05, max = 30.0) float sizeDegrees = 0.5f;

    /// A tint over the albedo photograph. White leaves the photograph's own
    /// colour, which is why it is the default.
    AFIELD() Assisi::Math::Color3 diskColor{1.0f, 1.0f, 1.0f};
    AFIELD(min = 0.0, max = 200.0) float diskIntensity = 2.0f;

    /// How much of the air's reddening a low moon takes, from none to all of it.
    ///
    /// **Not one, and that is a correction rather than a preference.** A low moon
    /// reddens exactly as hard as a low sun in physics — same air, same path —
    /// but real moonlight is about a millionth of sunlight, which is below where
    /// colour vision works, so almost none of that reddening is ever seen.
    /// @ref intensity above is raised four orders of magnitude to make night
    /// playable; this is the other half of that compromise, and without it a
    /// setting moon reads as a small sun.
    ///
    /// Hue only: the moon dims through the air by exactly as much at every value.
    /// One is the honest physics, for a project that wants it.
    AFIELD(min = 0.0, max = 1.0) float atmosphericTint = 0.2f;

    /// Simulated days from one new moon to the next.
    AFIELD(min = 1.0) float cycleDays = 29.53f;
    /// Where in that cycle day zero falls, in turns: 0 is new, 0.5 full. A quarter
    /// puts a half moon in the afternoon sky on the first day, which is what makes
    /// a daytime moon visible without anyone hunting for it.
    AFIELD(min = 0.0, max = 1.0) float phaseAtEpoch = 0.25f;

    /// Tilt of the orbit against the **ecliptic**, not the equator. Held a degree
    /// off a right angle, because an orbit inclined a full ninety degrees carries
    /// the moon through the ecliptic pole, which is the direction the disk image's
    /// up is taken from.
    AFIELD(min = 0.0, max = 89.0) float inclinationDegrees = 5.15f;

    /// Ecliptic longitude of the ascending node on day zero, from the March
    /// equinox.
    AFIELD(min = -180.0, max = 180.0) float nodeAtEpochDegrees = 90.f;

    /// How long the node takes to go once round, in years.
    ///
    /// **A node that does not move locks eclipses to two dates of the year**, and
    /// whenever the month divides the year that means the same alignment every
    /// year — eclipses always, or never. Letting it regress makes them emergent
    /// and rare with no eclipse code anywhere. Quoted in years because the node is
    /// a longitude in the ecliptic and the ecliptic's own period is the year.
    AFIELD(min = 0.01) float nodeCycleYears = 18.6f;
};

/// @brief The same clock with every lane finite and in range, and both cycles
/// wrapped into theirs.
[[nodiscard]] TimeOfDay Sanitized(TimeOfDay clock);
[[nodiscard]] Sun Sanitized(Sun sun);
[[nodiscard]] Moon Sanitized(Moon moon);

/// @brief Advance both clocks by @p dtSeconds of simulated time.
///
/// Each is held by its own freeze, so a frozen season under a running day is a
/// legitimate authoring view rather than an error. A non-finite or non-positive
/// step does nothing.
void AdvanceTimeOfDay(TimeOfDay &clock, float dtSeconds);

/// @brief Cut the clock to @p day and @p hour, and count it as a cut.
///
/// The hour wraps into [0, 24) carrying into the day. Returns false and changes
/// nothing for a non-finite hour, so a bad value from script cannot strand the
/// clock at a NaN that every downstream direction then inherits.
bool Jump(TimeOfDay &clock, int32_t day, double hour);

/// @brief Cut forward to the next occurrence of @p hour — today if it is still to
/// come, tomorrow otherwise. Sleeping until morning.
bool JumpForwardTo(TimeOfDay &clock, double hour);

/// @brief Cut the annual clock to @p dayOfYear, wrapping into the year.
///
/// The same serial as an hour cut, because a season cut moves the sun as far.
bool JumpSeason(TimeOfDay &clock, double dayOfYear);

/// @brief How fast the sun sweeps the sky, in radians per simulated second. Zero
/// while the daily clock is paused.
///
/// An upper bound rather than the exact rate: the sun's true speed is this times
/// the cosine of the declination, and a consumer budgeting ahead wants the bound.
[[nodiscard]] float SunAngularVelocity(const TimeOfDay &clock);

/// @brief The same for the moon, which lags the sun by one turn per lunation.
[[nodiscard]] float MoonAngularVelocity(const TimeOfDay &clock, const Moon &moon);

/// @brief The clock's two angles, as Render::Celestial takes them.
[[nodiscard]] Render::SkyClock ClockAngles(const TimeOfDay &clock);

/// @brief Where the sky is watched from, as Render::Celestial takes it.
[[nodiscard]] Render::Observer ObserverOf(const Sun &sun);

/// @brief The moon's orbit as of @p clock, with the node already regressed to it.
[[nodiscard]] Render::MoonOrbit OrbitOf(const TimeOfDay &clock, const Moon &moon);

} // namespace Assisi::Runtime
