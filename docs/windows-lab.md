# Windows GPU compatibility and Lab

## GPU detection and acceleration

The Windows native engine now keeps two inventories separate:

1. **ncnn/Vulkan devices** are the only devices that can be passed to the neural inference pipeline.
2. **DXGI display adapters** are the complete Windows hardware inventory. This includes older or
   integrated adapters such as GeForce GT 730 variants and Intel HD Graphics 4600, even when the
   installed driver does not expose a usable Vulkan device.

This avoids the previous ambiguous “no GPU” result and prevents a DXGI adapter index from being
mistaken for an ncnn Vulkan index. Auto mode still prefers a usable discrete Vulkan device, then an
integrated Vulkan device, then the multi-core CPU fallback. The process also exports the standard
NVIDIA Optimus and AMD PowerXpress high-performance preferences for hybrid systems.

A model can only run on a GPU when its Windows vendor driver exposes the Vulkan features required by
the bundled ncnn build. The exact GT 730 revision and Intel HD Graphics 4600 driver matter; the
application reports the adapter and the reason for a CPU fallback rather than claiming unsupported
hardware acceleration.

`enhancer-cli.exe --diag` and the **Diagnostics & Tests** window show both inventories, vendor IDs,
VRAM/shared-memory values, Vulkan visibility, and the native runtime summary.

## Lab

**Lab** is a separate, non-destructive WPF room opened from the main Studio header. It is fully
offline and does not alter the source file until the user saves a result.

### Included controls

- One-click **AI Portrait Restore (4×)** using the bundled LSDIR + existing SCRFD/CodeFormer native pipeline; it uses Auto Vulkan/CPU fallback and leaves the source file untouched.
- Exposure, contrast, highlights, shadows, black point, white point
- Temperature, tint, saturation, vibrance
- Clarity, sharpening, vignette, film grain, fade, monochrome
- Conservative local portrait retouch: skin smoothing, face light, eye detail and teeth whitening
- Five-point RGB tone curves plus shadow/highlight color-wheel hue and saturation controls
- Painted, normalized masks with two independent roles: a green protection mask for identity-critical
  areas and an orange retouch-area mask that scopes a stronger manual retouch. Both can be painted or
  erased directly over the preview.
- Strong local manual retouch (skin smoothing, face light, eye detail, teeth whitening and sharpening)
  stays a deterministic pixel operation; it is not the full AI face reconstruction path.
- Modern built-in looks: Natural Portrait, Portrait Glow, Clean Studio, Golden Hour, Cinematic Teal &
  Orange, Matte Film, HDR Detail Pop, and Fine Art Black & White
- Before/after toggle, fit/100%/200% preview, histogram summary, undo/redo and reset
- Batch apply the current look to multiple images, writing collision-safe `_Lab` outputs beside originals
- PNG and full-quality JPEG export

### Preset interchange

- `.enhancerpreset` and `.json` contain only the Lab values and can be copied between machines.
- `.xmp` import reads common Adobe Camera Raw / Lightroom names such as `Exposure2012`,
  `Contrast2012`, `Highlights2012`, `Shadows2012`, `Vibrance`, `Clarity2012`, `Temperature`, and
  `Tint`.
- `.xmp` export writes a small Camera Raw-compatible adjustment description. Photoshop can use it as
  a starting point. Enhancer's curves, color-wheel controls, face-protection switch, and normalized
  painted strokes are carried in the `enh:` extension namespace; Adobe-specific masks that are not
  represented by those strokes are not guessed or silently misapplied.
