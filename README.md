# StepVR OpenVR Detour

Minimal OpenVR detour project for injecting stepper-based locomotion input into VR games.

This project runs a payload DLL inside the target game process and hooks the OpenVR calls the game already uses.

The current goal is simple:

- read forward locomotion input from an external source
- inject it into the OpenVR input path already used by the game
- return the modified input to the game
- avoid creating a separate custom SteamVR device

---

## Purpose

Many VR games ignore custom SteamVR drivers or unusual controller roles.

Instead of creating a new tracked device, this project modifies the OpenVR input data already consumed by the game.

At the moment, the locomotion signal comes from a fitness stepper connected to an ESP32 Atom Lite with a hall sensor.

The current prototype is focused on a Godzilla game, where the stepper motion fits the movement style well.

---

## Current Approach

The OpenVR runtime itself is not replaced.

This project injects a payload DLL into the target game process and hooks the relevant OpenVR interfaces from inside the game.

The current input path is primarily based on:

`IVRInput`

More specifically, the project hooks the action-based input flow and overrides the forward movement analog action for the left hand.

The project also still includes a `IVRSystem::GetControllerState` hook for inspection and debugging of raw controller state.

---

## Current Input Flow

External stepper / HID writer  
→ shared forward ingress state  
→ payload DLL inside game process  
→ `IVRInput` detours  
→ `/actions/default/in/movejoystick` for `/user/hand/left`  
→ modified analog locomotion returned to the game

---

## What Is Currently Hooked

### IVRInput path

The payload currently hooks:

- `GetActionSetHandle`
- `GetActionHandle`
- `GetInputSourceHandle`
- `UpdateActionState`
- `GetAnalogActionData`
- `GetDigitalActionData`

The forward override is applied on:

`/actions/default/in/movejoystick`

restricted to:

`/user/hand/left`

If a valid forward ingress snapshot is available, the DLL replaces the analog action data with the external forward value.

### IVRSystem path

The repository also still hooks:

`IVRSystem::GetControllerState`

This is currently useful for logging, inspection, and reverse engineering of raw controller state, but the main locomotion injection path is now the `IVRInput` action system.

---

## Repository Layout

`hooking_library/`  
payload DLL code and OpenVR detours

`launcher/`  
combined runtime host for device polling, shared memory, and DLL injection

`injector/`  
legacy DLL injector executable

`deps/openvr/`  
OpenVR SDK dependency

`deps/minhook/`  
MinHook dependency

---

## Components

### Launcher EXE

The preferred runtime entrypoint is now the launcher executable.

It is responsible for:

- polling the WinMM stepper / joystick device
- writing the current forward value into shared memory
- finding the target game process
- injecting `stepvr_detour.dll`
- staying alive so the writer heartbeat keeps updating

### Payload DLL

The payload DLL runs inside the target game process.

It is responsible for:

- waiting for `openvr_api.dll`
- obtaining the real OpenVR interfaces
- installing the input detours
- reading the external forward ingress state
- overriding the matching OpenVR movement action
- logging observed input values for debugging

### Injector EXE

The repository also still builds a companion injector executable.

It searches for the target process and injects `stepvr_detour.dll` with `LoadLibraryA`.

By default it looks for:

`Monster Titans Playground`

You can also pass a different process name as the first command line argument.

This tool is now mainly a legacy fallback. The normal path is to run the launcher instead.

---

## Build Requirements

- Windows
- Visual Studio
- CMake
- C++17 compiler

---

## Build

```bash
cmake -S . -B build
cmake --build build --config Release
