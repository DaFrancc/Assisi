/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/Buffer.hpp>

#include <Assisi/Core/Logger.hpp>

#include <algorithm>

namespace Assisi::Render
{

void Buffer::Create(nvrhi::IDevice *device, uint32_t elementStride, uint32_t capacityElements,
                    bool allowUnorderedAccess, const char *debugName)
{
    _elementStride = elementStride;
    _capacityElements = capacityElements;
    _debugName = debugName != nullptr ? debugName : "Render::Buffer";
    _overflowWarned = false;

    nvrhi::BufferDesc desc;
    desc.byteSize = static_cast<uint64_t>(elementStride) * capacityElements;
    desc.structStride = elementStride;
    desc.canHaveUAVs = allowUnorderedAccess;
    desc.debugName = debugName != nullptr ? debugName : "Render::Buffer";
    desc.initialState = allowUnorderedAccess ? nvrhi::ResourceStates::UnorderedAccess
                                              : nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    _buffer = device->createBuffer(desc);
}

void Buffer::Upload(nvrhi::ICommandList *commandList, const void *data, uint32_t elementCount) const
{
    const uint32_t count = std::min(elementCount, _capacityElements);
    if (elementCount > _capacityElements && !_overflowWarned)
    {
        Core::Log::Warn("Render::Buffer \"{}\": upload of {} elements exceeds capacity {}; "
                        "{} dropped. Increase the buffer's capacity. (Warned once.)",
                        _debugName, elementCount, _capacityElements, elementCount - _capacityElements);
        _overflowWarned = true;
    }
    if (count == 0)
    {
        return;
    }
    commandList->writeBuffer(_buffer, data, static_cast<size_t>(count) * _elementStride);
}

void Buffer::ClearToZero(nvrhi::ICommandList *commandList) const
{
    commandList->clearBufferUInt(_buffer, 0u);
}

} // namespace Assisi::Render
