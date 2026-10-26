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
