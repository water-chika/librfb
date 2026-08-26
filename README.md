# LibRFB

Library for Remote Frame Buffer protocol.

## Supported Encodings

1. Raw
2. ZRLE
3. H264

## rfb window demo

`rfb_window_demo` renders to a Wayland surface using DRM (amdgpu BOs exported as
dma-buf) shared with ROCm/HIP via external memory. Vulkan is no longer used.

## Reference

[rfbproto](https://github.com/rfbproto/rfbproto)
