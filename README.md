# EntangleSim

A real-time 3D visualization of quantum entanglement, built in C++ with OpenGL. Two particles are linked in a singlet entanglement state — measuring one instantly collapses both to opposite spins, and the demo renders the particles, their spin arrows, and motion trails live in a navigable 3D scene.

## How it works

- Two particles (`Object`s) orbit each other along independent Lissajous-style paths, rendered as shaded spheres with motion trails.
- Each particle has a spin (`Up`, `Down`, or `Unknown`) shown as a colored arrow above it: green for up, yellow for down, faint white for unmeasured.
- While entangled, both spins stay `Unknown` until a measurement is made. Measuring particle 1 collapses both particles to opposite spins (singlet correlation), matching the quantum mechanics of an entangled pair.
- Particle identity (electron / proton / positron) is shown by sphere color and can be changed live.

## Controls

| Key | Action |
| --- | --- |
| `W` `A` `S` `D` | Move camera |
| `Space` / `Left Ctrl` | Move camera up / down |
| Mouse | Look around |
| Scroll | Zoom |
| `F11` | Toggle fullscreen |
| `P` | Pause / resume |
| `M` | Measure particle 1 (random outcome) |
| `U` | Measure particle 1 as Spin Up |
| `J` | Measure particle 1 as Spin Down |
| `R` | Reset entanglement (both particles back to Unknown) |
| `1` / `2` / `3` | Set particle 1 type (Electron / Proton / Positron) |
| `Shift` + `1` / `2` / `3` | Set particle 2 type |
| `Esc` | Quit |

## Tech stack

- **C++** (Visual Studio project, `x64`)
- **OpenGL 3.3** (core profile)
- **GLFW** — windowing and input
- **GLEW** — OpenGL extension loading
- **GLM** — vector/matrix math

## Project layout

```
Render3D/
├── Render3D.slnx              # Visual Studio solution
├── Dependencies/               # Vendored libraries (GLFW, GLEW, GLM, SOIL2)
└── Render3D/
    └── simulator.cpp           # Entanglement simulation, rendering, input, UI
```

## Building

1. Open `Render3D.slnx` in Visual Studio 2022 (or newer).
2. Select the `Debug` or `Release` configuration for `x64`.
3. Build and run — required DLLs (`glew32.dll`, `glfw3.dll`) are included alongside the project.
