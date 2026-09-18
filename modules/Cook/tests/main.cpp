/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <Assisi/Runtime/SceneSerializer.hpp>

int main(int argc, char **argv)
{
    // A level cooks by being loaded, and the fixture tree is source text.
    (void)Assisi::Runtime::SceneSerializer::SetDocumentReader(&Assisi::Runtime::SceneSerializer::ReadTextDocument);
    return doctest::Context(argc, argv).run();
}
