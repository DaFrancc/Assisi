// One 256-point inverse FFT of two pairs of complex fields at once, in shared
// memory, by a workgroup of 128: each run combines one pair of points per stage.
// Load each point at BitReversed(its index), call InverseFftShared, then read
// the results in order. Render::InverseFft is this on the CPU.

const uint kFftRuns = kWaterFftSize / 2u;

shared vec4 sFields01[kWaterFftSize];
shared vec4 sFields23[kWaterFftSize];

uint BitReversed(uint index)
{
    return bitfieldReverse(index) >> (32u - kWaterFftLog2);
}

vec4 MultiplyPair(vec4 pair, vec2 twiddle)
{
    return vec4(ComplexMultiply(pair.xy, twiddle), ComplexMultiply(pair.zw, twiddle));
}

void InverseFftShared(uint run)
{
    barrier();
    for (uint span = 2u; span <= kWaterFftSize; span <<= 1u)
    {
        uint  halfSpan = span >> 1u;
        uint  offset   = run % halfSpan;
        uint  first    = (run / halfSpan) * span + offset;
        uint  second   = first + halfSpan;
        float angle   = kTwoPi * float(offset) / float(span);
        vec2  twiddle = vec2(cos(angle), sin(angle));

        vec4 even01 = sFields01[first];
        vec4 odd01  = MultiplyPair(sFields01[second], twiddle);
        vec4 even23 = sFields23[first];
        vec4 odd23  = MultiplyPair(sFields23[second], twiddle);
        sFields01[first]  = even01 + odd01;
        sFields01[second] = even01 - odd01;
        sFields23[first]  = even23 + odd23;
        sFields23[second] = even23 - odd23;
        barrier();
    }
}
