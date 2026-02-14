# Assisi Engine

Assisi is a modern C++ game engine for Windows and Linux focused on building performant, modular systems for
real-time games. This project is primarily for my own education and for my own
needs first. Suggestions, PRs, forks, and bug reports are welcome. Use of this
codebase or engine in your own projects or forks is 100% welcome and free of charge if in compliance
with the license. I kindly ask that you credit the engine and myself 
(Francisco Vivas Puerto aka DaFrancc) in both the repo and any games you make with this engine. Credit is not required, but it would be greatly appreciated.

## Getting Started
### 1. Clone the repo.
```bash
git clone https://github.com/DaFrancc/Assisi.git
```

### 2. Install Dependencies
#### Tools:
- CMake
- Conan
#### Libraries (installed in next step):
- GLFW
- GLM
- Assimp
- StbImage
- Glad

### 3. Initialization
#### Windows:
- Run ```setup.ps1``` and follow the instructions to create your game directory and install dependencies using [Conan](https://conan.io/).
```bash
powershell -ExecutionPolicy Bypass -File .\setup.ps1
  ```
 - Note: If you do not have the dependencies already installed and built, this process will take several minutes depending on your internet speed and your hardware. If you have already run the script once successfully, running it again will only take seconds.
#### Linux:
- WIP
#### MacOS:
- Currently unsupported, but you are free to fiddle around with it and submit a pull request.

## Understanding Assisi's Module Layout
Assisi is organized into several modules, each responsible for a specific aspect of the engine. Below is an overview of the main modules and their responsibilities:
### Core
The Core module contains the fundamental building blocks of the engine, including the main application loop, event handling, and basic utilities. 
It serves as the backbone of the engine and provides essential functionality for all other modules.

### Math
The Math module provides a collection of mathematical functions and data structures commonly used in game development, such as vectors, matrices, and quaternions.
Currently, it is mostly a wrapper around the GLM library, but in the future it will include more custom math utilities and optimizations specific to the engine's needs.

### Window
The Window module is responsible for creating and managing the application window, handling input events, and interfacing with the underlying operating system's windowing system.
It abstracts away platform-specific details and provides a unified interface for window management across different platforms. <br><br>
You will most likely never touch this module directly, as it is used internally by the Core module to create and manage the application window.
However, you are certainly free to use it directly if you need more control over the windowing system or if you want to implement custom window behavior.

## Render
The Render module is responsible for rendering graphics to the screen. It provides an abstraction layer over the underlying graphics API (currently OpenGL, 
but in the future it may support other APIs like Vulkan or DirectX). Hopefully in the future it will be as customizable as Unity's rendering pipeline, 
allowing you to create custom render passes, shaders, and materials to achieve the exact look you want for your game.

## ECS
The ECS (Entity-Component-System) module provides a framework for working with entity IDs, component storage, sparse sets, queries, etc.
It is designed to be flexible and efficient, allowing you to create complex game objects and systems without worrying about the underlying data structures or performance implications.

## Runtime
The Runtime module mostly contains a bunch of useful prefabs, components, and systems that you can use out of the box to get your game up and running quickly. 
It includes things like a basic camera controller, spawning systems, and other things that are common in every game, but don't necessarily belong in the core engine. 
It is meant to be a collection of reusable game logic and functionality that can be easily integrated into your projects.
They themselves do not make up a whole game, but they are meant to be building blocks that you can use to create your own unique games.

## Documentation
<!-- docs-block: start -->
- [Getting Started](<add-docs-link>)
- [Build & Tooling](<add-docs-link>)
- [Architecture Overview](<add-docs-link>)
- [API Reference](<add-docs-link>)
<!-- docs-block: end -->

## Useful Links
<!-- links-block: start -->
- [Roadmap](<add-link>)
- [Issue Tracker](<add-link>)
- [Community/Support](<add-link>)
<!-- links-block: end -->
