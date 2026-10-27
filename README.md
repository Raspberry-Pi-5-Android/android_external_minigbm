# android_external_minigbm

AOSP minigbm (graphics buffer allocator) with Raspberry Pi 5 support

## Changes from AOSP

- Dumb buffers for the vc4 display driver on BCM2711/BCM2712
- The allocator drops DRM master after opening a primary node
- GPU-only buffers use the V3D UIF tiled layout
