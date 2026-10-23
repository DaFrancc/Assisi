/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Editor/EditorEvents.hpp>

#include <Assisi/Core/EventScope.hpp>

#include <typeinfo>

namespace Assisi::Editor
{

void DeclareEditorEvents()
{
    Assisi::Core::DeclareUniversal(typeid(EntitySelectionChangedEvent));
}

} // namespace Assisi::Editor
