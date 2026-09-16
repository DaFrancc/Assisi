/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <Assisi/Runtime/SceneSerializer.hpp>

int main(int argc, char **argv)
{
    // The editor reads levels and blueprints as the JSON an author saves.
    (void)Assisi::Runtime::SceneSerializer::SetDocumentReader(&Assisi::Runtime::SceneSerializer::ReadTextDocument);
    return doctest::Context(argc, argv).run();
}
