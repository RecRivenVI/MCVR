# Minecraft Vulkan Renderer
Minecraft Vulkan Renderer (MCVR) is a C++ implemented render framework for minecraft. It should be used with the Radiance mod.



# Build

First, make sure submodules are cloned with git.

```
git submodule update --init --recursive
```

## Linux

Use `cmake` to configure the project.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DJAVA_PROJECT_ROOT_DIR=${PATH_TO_RADIANCE_JAVA_PROJECT} -DUSE_AMD=ON -DMCVR_ENABLE_NRD=ON
```

Build and install.

```
cmake --build build -j
cmake --install build
```

## Windows

Use a Visual Studio generator to configure the project. The complete Windows
build enables FidelityFX and NRD; Ninja is not supported for that combination
because FidelityFX's platform selection is incompatible with NRD's nested
ShaderMake configure.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DJAVA_PROJECT_ROOT_DIR=${PATH_TO_RADIANCE_JAVA_PROJECT} -DMCVR_ENABLE_NRD=ON -DUSE_AMD=ON
```

Build and install.

```
cmake --build build -j --config Release
cmake --install build --config Release
```

Radiance's `prepareRuntime` task selects the default installed Visual Studio
generator automatically. Override it only when necessary with
`-Pmcvr.cmakeGenerator="Visual Studio 17 2022"`, and use a fresh MCVR build
directory when changing generators.
