/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/SessionTypes.hpp>

namespace Assisi::App
{

std::string_view Name(HostFailure reason)
{
    switch (reason)
    {
    case HostFailure::PortUnavailable:            return "PortUnavailable";
    case HostFailure::NoLevel:                    return "NoLevel";
    case HostFailure::TooMuchData:                return "TooMuchData";
    case HostFailure::BadPassword:                return "BadPassword";
    case HostFailure::AlreadyInSession:           return "AlreadyInSession";
    case HostFailure::DirectConnectionsForbidden: return "DirectConnectionsForbidden";
    case HostFailure::Count_:                     break;
    }
    return "Unknown";
}

std::string_view Name(JoinRefusal reason)
{
    switch (reason)
    {
    case JoinRefusal::WrongSecret:                return "WrongSecret";
    case JoinRefusal::Full:                       return "Full";
    case JoinRefusal::TooManyTries:               return "TooManyTries";
    case JoinRefusal::Version:                    return "Version";
    case JoinRefusal::Content:                    return "Content";
    case JoinRefusal::Level:                      return "Level";
    case JoinRefusal::NotFound:                   return "NotFound";
    case JoinRefusal::Unreachable:                return "Unreachable";
    case JoinRefusal::AlreadyInSession:           return "AlreadyInSession";
    case JoinRefusal::DirectConnectionsForbidden: return "DirectConnectionsForbidden";
    case JoinRefusal::Count_:                     break;
    }
    return "Unknown";
}

std::string_view Name(EndReason reason)
{
    switch (reason)
    {
    case EndReason::Left:           return "Left";
    case EndReason::HostLeft:       return "HostLeft";
    case EndReason::ConnectionLost: return "ConnectionLost";
    case EndReason::Count_:         break;
    }
    return "Unknown";
}

} // namespace Assisi::App
