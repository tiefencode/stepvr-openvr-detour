# StepVR OpenVR Detour

Minimal OpenVR detour DLL used to intercept and wrap controller input inside a running VR game.

This project injects a small payload into the game process and hooks the OpenVR controller state function used by the game.

The original function is always called first.
The returned controller state can then be inspected or modified.

---

## Purpose

Many VR games ignore custom SteamVR drivers or controller roles.

Instead of creating a new device, this project modifies the controller state that the game already reads.

The hook wraps the OpenVR call:

`IVRSystem::GetControllerState`

The detour performs the following steps:

1. call the original function
2. capture the returned `VRControllerState_t`
3. inspect or modify values
4. return the modified state to the game

---

## Scope

This repository contains a minimal payload DLL that:

* obtains a pointer to `IVRSystem`
* locates `GetControllerState` in the vtable
* stores the original function pointer
* installs a detour
* wraps the original call
* logs controller state values

Future versions may override controller axes to inject locomotion input.

---

## Architecture

Game
→ OpenVR runtime
→ `IVRSystem::GetControllerState`
→ StepVR detour wrapper
→ original function call
→ modified controller state returned to the game

The OpenVR runtime itself is not replaced or proxied.

---

## Building

Requirements:

* Windows
* Visual Studio 2022
* CMake 3.20 or newer

Basic workflow:

1. configure with CMake
2. build the Release configuration
3. produce the payload DLL

---

## Testing

1. inject the DLL into the running game process
2. start the game normally
3. verify that the log file appears
4. confirm that controller state calls are captured

If successful, controller axis values will appear in the log.

---

## Repository Structure

`CMakeLists.txt`
build configuration

`dllmain.cpp`
payload entry point

`detour_controller_state.cpp`
OpenVR controller state wrapper

`shared.h / shared.cpp`
logging and shared utilities
