# OpenGL PyroWave encoder

Compute-shader encoder for [PyroWave](https://github.com/Themaister/pyrowave). The context request is OpenGL 4.3 core and the shaders are GLSL 430. That is inside the Radeon HD 6670's OpenGL 4.4 (Catalyst 14.4 / GLCTS 4.4). The encoder does not ask for OpenGL 4.5 or 4.6, shader subgroups, explicit 16-bit types, or 8-bit shader storage. The DWT, quantizer, and block packer follow the subgroup-free shaders in [pyrowave-dx11](https://github.com/Lyamc/pyrowave-dx11), which themselves follow `shaders/dwt.comp`, `shaders/wavelet_quant.comp`, and `shaders/block_packing.comp` at upstream `a0d2e71`. This tree is upstream PyroWave at the checkout used to write the port. Those three shaders are unchanged between `a0d2e71` and current `master`.

Rate control is quant 0. That is the same choice as pyrowave-dx11 v0.1.1: a nonzero quant from the `cs_5_0` resolve pass drops every plane. The packet bytes are a PyroWave record (sequence header, then block packets) that `libpyrowave` packetizes the same way.

## Build

```text
make -C gl
gl/build/pwgl_test
```

`clang++` or `g++` with C++17. The shared library is `gl/build/libpyrowave_gl.so`. It `dlopen`s `libEGL.so.1`, or `/run/opengl-driver/lib/libEGL_nvidia.so.0` on NixOS, and creates a surfaceless 4.3 core context (`EGL_EXT_platform_device`, with a 16x16 pbuffer fallback). Software renderers (`llvmpipe`, `softpipe`, `SwiftShader`) are rejected unless `flags` bit 0 is set.

`pwgl_test` encodes a 64x64 frame and a 320x240 frame and checks the sequence header.

## ABI

`pwgl_create_device`, `pwgl_encoder_create`, `pwgl_encoder_encode_cpu`, `pwgl_encoder_compute_num_packets`, `pwgl_encoder_packetize`. Input is 8-bit `YUV420P` or `YUV444P`. `PWGL_API_VERSION` is 1.

The encoder keeps one GL context for the process. Destroy the encoder before the device.

On Windows the context is WGL, OpenGL 4.3 core, on a hidden 16×16 window. The DLL and the process that loads OpenGL export `NvOptimusEnablement` so a machine with two GPUs creates that context on the high-performance adapter. Build from an x64 developer prompt:

```text
cl /nologo /O2 /EHsc /std:c++17 /MT /W4 /D_CRT_SECURE_NO_WARNINGS /DPWGL_BUILD_DLL /Iinclude /Isrc src\gl_api.cpp src\encoder.cpp /LD /Fe:build\pyrowave_gl.dll /link opengl32.lib user32.lib gdi32.lib
cl /nologo /O2 /EHsc /std:c++17 /MT /W4 /D_CRT_SECURE_NO_WARNINGS /Iinclude src\cmp_bench.cpp /Fe:build\pwgl_cmp.exe /link psapi.lib
```

`pwgl_cmp` is the same measurement as pyrowave-dx11 `tools/pwcmpbench.cpp`: the synthetic I420 frame, rate cap `max(W*H*3/16, 32 KiB)`, 20 warmup frames, then at least 200 frames and 5 seconds. The packet buffer is 64 MiB.

## Performance

HYTE (192.168.0.30), 2026-10-26. i9-13900K, NVIDIA GeForce RTX 3060. The OpenGL renderer string is `NVIDIA GeForce RTX 3060/PCIe/SSE2`. Lavapipe is `llvmpipe (LLVM 23.1.2, 512 bits)`. Vulkan asked for vendor `0x10DE`. D3D11 used the NVIDIA adapter. Each cell is min / avg / max / std. CPU is this process's share of 32 logical processors. The D3D11 and Vulkan writeup, one-core CPU, and private bytes are in pyrowave-dx11 `comparison.md`.

1280×720, cap 172,800 bytes:

| Encoder | Latency (ms) | CPU (%) | Working set (MiB) | Output bytes |
|---|---|---|---|---|
| lavapipe | 19.76 / 21.69 / 24.64 / 0.86 | 6.3 / 23.5 / 45.5 / 7.8 | 101.1 / 101.1 / 101.1 / 0.0 | 172,740 |
| Vulkan | 0.42 / 0.54 / 2.68 / 0.15 | 0.0 / 1.4 / 3.9 / 0.9 | 198.2 / 198.6 / 198.7 / 0.1 | 172,760 |
| OpenGL | 2.53 / 2.64 / 3.84 / 0.11 | 3.2 / 5.8 / 6.5 / 0.7 | 178.0 / 178.7 / 178.7 / 0.1 | 1,324,828 |
| D3D11 | 0.27 / 0.34 / 2.10 / 0.12 | 1.6 / 4.3 / 6.2 / 1.0 | 45.6 / 45.7 / 45.7 / 0.0 | 41,988 |

1920×1080, cap 388,800 bytes:

| Encoder | Latency (ms) | CPU (%) | Working set (MiB) | Output bytes |
|---|---|---|---|---|
| lavapipe | 30.55 / 33.54 / 36.47 / 1.05 | 6.3 / 27.3 / 49.5 / 8.6 | 117.5 / 117.6 / 118.6 / 0.1 | 388,720 |
| Vulkan | 0.74 / 0.88 / 3.05 / 0.17 | 0.0 / 1.1 / 2.9 / 1.0 | 203.7 / 204.0 / 204.1 / 0.1 | 388,676 |
| OpenGL | 3.99 / 4.19 / 4.93 / 0.14 | 3.3 / 5.7 / 7.8 / 0.7 | 231.8 / 231.9 / 232.8 / 0.3 | 2,831,224 |
| D3D11 | 0.42 / 0.54 / 3.02 / 0.19 | 1.6 / 4.0 / 6.2 / 0.9 | 50.7 / 50.8 / 50.8 / 0.0 | 79,964 |

Lavapipe and Vulkan fill the cap. D3D11 is quant 0 and keeps 60 blocks below width 1600 and 96 blocks at 1080p (41,988 and 79,964 bytes). This OpenGL encoder is quant 0 and packs the whole pyramid (1,324,828 and 2,831,224 bytes). The OpenGL averages are 2.64 ms at 720p (about 380 fps) and 4.19 ms at 1080p (about 240 fps) for that larger bitstream.
