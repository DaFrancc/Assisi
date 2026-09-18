/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <Assisi/Core/ConfigReader.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

int main(int argc, char **argv)
{
    // These tests write levels, blueprints and configs into asset roots as JSON
    // text, the way the editor reads them.
    (void)Assisi::Runtime::SceneSerializer::SetDocumentReader(&Assisi::Runtime::SceneSerializer::ReadTextDocument);
    (void)Assisi::Core::SetConfigReader(&Assisi::Core::ReadTextConfig);
    return doctest::Context(argc, argv).run();
}
