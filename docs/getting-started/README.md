# Getting started

| Step | Page |
|---|---|
| 1. Build | [Build](build.md) |
| 2. Run the examples | [Examples](../../examples/README.md): start with `00-hello-voxel` |
| 3. Write your own program | [Your first app](first-app.md) |
| 4. How it works | [Concepts](../concepts/README.md) |

```bash
sudo dnf install gcc-c++ cmake git glfw-devel wayland-devel     # Fedora
git clone https://github.com/ajsuper/ProjectV.git && cd ProjectV
git submodule update --init --recursive
cmake --preset dev && cmake --build --preset dev
cd build/examples/hello_voxel && ./hello_voxel
```
