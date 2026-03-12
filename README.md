# StepVR OpenVR Detour

Minimal OpenVR detour project for injecting locomotion input into VR games.

This project wraps the controller state call already used by the game and modifies the returned input values.

The goal is simple:

- find the controller state field used for forward movement
- combine it with custom HID / stepper input
- write the modified value back
- return the updated controller state to the game

---

## Purpose

Many VR games ignore custom SteamVR drivers or special controller roles.

Instead of creating a new device, this project modifies the controller state that the game already reads.

The hook wraps:

`IVRSystem::GetControllerState`

The detour always:

1. calls the original function
2. captures the returned `VRControllerState_t`
3. inspects buttons and axis values
4. modifies the relevant movement value
5. returns the updated state to the game

---

## Approach

The OpenVR runtime itself is not replaced.

This project works by running a payload DLL inside the target game process and wrapping the real OpenVR controller state call.

Custom HID / stepper input is then merged into the same controller state structure already used by the game.

---

## Architecture

Game  
→ OpenVR runtime  
→ `IVRSystem::GetControllerState`  
→ StepVR detour wrapper  
→ original function call  
→ custom locomotion value merged into `VRControllerState_t`  
→ modified controller state returned to the game

---

## Repository Layout

`hooking_library/`  
payload DLL code

`injector/`  
companion executable target

`deps/openvr/`  
OpenVR SDK dependency

`deps/minhook/`  
MinHook dependency

---

## Components

### Payload DLL

The payload DLL runs inside the target game process.

It is responsible for:

- obtaining a real `IVRSystem*`
- installing the `GetControllerState` hook
- reading controller state values
- merging custom locomotion input
- returning the modified controller state

### Injector EXE

The repository also builds a companion executable target used together with the payload DLL.

---

## Build Requirements

- Windows
- Visual Studio 2022
- CMake 3.20 or newer

---

## Build Output

The project produces:

- `stepvr_detour.dll`
- `stepvr_injector.exe`