# DXGI Desktop Mirror

Low-latency display mirroring with perfect vsync output. Supports HDR to SDR tonemapping.

## Architecture

**Capture thread**: Blocks on `AcquireNextFrame`, wakes at source refresh rate (60/120/144Hz)

**Main thread**: Renders with VSync (`Present(1, 0)`), outputs at target refresh rate

Triple-buffered staging textures ensure lock-free operation with no flicker. Uses waitable swap chain for optimal frame pacing when source and target refresh rates differ (e.g., 120Hz → 60Hz).

**Smart frame selection**: When source rate > target rate, the render thread waits for a specific frame ID (last + N) rather than using a fixed delay. This guarantees consistent frame skipping (e.g., Skip:2-2 for 120Hz→60Hz) regardless of system timing variations.

## HDR Support

When the source monitor is HDR (DXGI_FORMAT_R16G16B16A16_FLOAT / scRGB), the program automatically applies **maxRGB Reinhard tonemapping** to convert to SDR for display on SDR monitors.

- Uses `IDXGIOutput5/6::DuplicateOutput1` to capture actual HDR frames
- maxRGB Reinhard preserves SDR content (values ≤1.0) and only compresses HDR highlights
- Tonemapping is enabled by default
- Use `--no-tonemap` to disable (output will be clipped/washed out)
- Use `--sdr-white N` to adjust the SDR white level (default: 240 nits)

References:
- [OBS Studio color.effect](https://github.com/obsproject/obs-studio/blob/master/libobs/data/color.effect)

## Expected Stats

```
Out: 60 Cap: 60 Uniq: 60 Dup:  0 Drop:  0 Skip:1-1(1.0)   (60Hz → 60Hz)
Out: 60 Cap:120 Uniq: 60 Dup:  0 Drop: 60 Skip:2-2(2.0)   (120Hz → 60Hz, perfect)
Out: 60 Cap:120 Uniq: 60 Dup:  0 Drop: 60 Skip:1-3(2.0)   (120Hz → 60Hz, inconsistent)
```

- **Out** - Frames presented (matches target refresh rate)
- **Cap** - Frames captured (matches source refresh rate)
- **Uniq** - Unique frames displayed
- **Drop** - Captured frames skipped (expected when source > target)
- **Skip:min-max(avg)** - Frame skip delta between displayed frames
  - For 120Hz→60Hz, ideal is `Skip:2-2(2.0)` (consistent every-other-frame)
  - `Skip:1-3(2.0)` means inconsistent pacing (causes micro-stutter)

## Build

```
cl /O2 /EHsc main.cpp /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib winmm.lib
```

## Usage

```
dxgi-mirror.exe [options]

  --source N       Source monitor (default: 0)
  --target N       Target monitor (default: 1)
  --stretch        Stretch to fill (ignore aspect ratio)
  --no-tonemap     Disable HDR to SDR tonemapping
  --sdr-white N    SDR white level in nits (default: 240)
  --no-cursor      Hide the mouse cursor
  --no-waitable    Disable waitable swap chain (frame pacing)
  --no-smart-select Disable smart frame selection (use fixed delay)
  --no-frame-delay Disable frame delay (frame pacing fallback)
  --frame-delay N  Frame delay in µs (default: 1000 = 1ms, fallback only)
  --list           List monitors
```

Press **ESC** or **CTRL+C** to exit gracefully.

## License

MIT
