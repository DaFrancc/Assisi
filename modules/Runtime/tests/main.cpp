/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <Assisi/Runtime/SceneSerializer.hpp>

int main(int argc, char **argv)
{
    // These tests write levels and blueprints into asset roots as JSON text, the
    // way the editor reads them.
    (void)Assisi::Runtime::SceneSerializer::SetDocumentReader(&Assisi::Runtime::SceneSerializer::ReadTextDocument);
    return doctest::Context(argc, argv).run();
}
