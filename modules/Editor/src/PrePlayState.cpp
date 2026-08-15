/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/PrePlayState.hpp>

namespace Assisi::Editor
{

PrePlayState CapturePrePlayState(const std::string &levelPath, const std::vector<std::string> &systemNames,
                                 const Runtime::InstanceTable &instances)
{
    return PrePlayState{.levelPath = levelPath, .systemNames = systemNames, .instances = instances};
}

bool RestorePrePlayState(const PrePlayState &captured, std::string &levelPath,
                         const std::vector<std::string> &liveSystemNames, Runtime::InstanceTable &instances)
{
    levelPath = captured.levelPath;
    // Assigned whole, not rebuilt row by row: a rebuild through Add() would
    // renumber everything, and one through Clear() + RestoreAt() would put the
    // rows back at their ids but leave the allocator at the highest *surviving*
    // id — so an instance deleted before Play (its id still spoken for by an
    // undoable transaction) would have that id handed out again.
    instances = captured.instances;
    return liveSystemNames != captured.systemNames;
}

} // namespace Assisi::Editor
