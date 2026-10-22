/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/BodyReport.hpp>

namespace Assisi::NetSync
{

void WriteBodyReports(std::span<const BodyReport *const> reports, Core::BitWriter &writer)
{
    writer.WriteVarUInt32(static_cast<std::uint32_t>(reports.size()));
    for (const BodyReport *report : reports)
    {
        writer.WriteVarUInt64(report->tick);
        writer.WriteVarUInt32(static_cast<std::uint32_t>(report->bodies.size()));
        for (const BodyState &body : report->bodies)
        {
            WriteBodyState(body, writer);
        }
    }
}

bool ReadBodyReports(Core::BitReader &reader, std::vector<BodyReport> &out)
{
    // Bounded before either count is used as one: both come off the wire.
    const std::uint32_t ticks = reader.ReadVarUInt32();
    if (!reader.Ok() || ticks > kMaxReportTicks)
    {
        return false;
    }
    std::vector<BodyReport> decoded(ticks);
    for (BodyReport &report : decoded)
    {
        report.tick = reader.ReadVarUInt64();
        const std::uint32_t bodies = reader.ReadVarUInt32();
        if (!reader.Ok() || bodies > kMaxReportedBodies)
        {
            return false;
        }
        report.bodies.resize(bodies);
        for (BodyState &body : report.bodies)
        {
            if (!ReadBodyState(reader, body))
            {
                return false;
            }
        }
    }
    out.insert(out.end(), decoded.begin(), decoded.end());
    return true;
}

} // namespace Assisi::NetSync
