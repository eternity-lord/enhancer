# Photographic SR model evaluation

## Decision

The default photographic backbone is now **4xLSDIR** (`lsdir-photo-x4.{param,bin}`), distributed by the NCNN model mirror in [upscayl/custom-models](https://github.com/upscayl/custom-models) and credited to **Philip Hofmann / Phhofm**. The model author describes 4xLSDIR as an RRDBNet trained on paired 4x-downsampled LSDIR photographs. The model release specifies **CC BY 4.0**; the application credits the author and license in its About screen.

The previous `realesrgan-x4plus` pair remains bundled and selectable as **Real-ESRGAN Photo 4x (Conservative Fallback)**. This preserves a known-good photographic option while making the quality-tested model the default for the existing `PHOTO_X4` setting and for Prime, Ultra, and Old Photo presets.

## Compatibility checks

- The candidate is a standard NCNN `.param` / `.bin` pair with input blob `data`, output blob `output`, scale 4, and the same 999-layer / 1782-blob RRDB graph shape as the existing photographic model.
- The pair loaded successfully with ncnn `1.0.20261020` on CPU and produced the expected 4x output for every benchmark image.
- The bundled NCNN pair is the compact weight encoding used by the existing app assets (33,424,520-byte `.bin`). The author’s 66,793,352-byte FP32 pair was also loaded and compared; on the Lena portrait its decoded output differed by only 0.37 mean absolute 8-bit levels (0.77 RMSE) after JPEG export. The compact pair was selected because the visible gain from shipping FP32 weights was negligible while static model memory and package size approximately doubled.

### Provenance and hashes

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `app/src/main/assets/models/lsdir-photo-x4.bin` | 33,424,520 | `0622f182f0a940b395e4fc70e2707b285e016fa4b014e855205eb40efddfb853` |
| `app/src/main/assets/models/lsdir-photo-x4.param` | 108,039 | `4576ed5c2fc5fa250d3c3d585ef02248f26abdfc1867088078f501fe71e5d61e` |

Reference release metadata: [Phhofm 4xLSDIR README](https://github.com/Phhofm/models/blob/main/4xLSDIR/README.md), [OpenModelDB 4x-LSDIR record](https://github.com/OpenModelDB/open-model-database/blob/main/data/models/4x-LSDIR.json), and the NCNN pair mirror [upscayl/custom-models](https://github.com/upscayl/custom-models/tree/main/models).

## Benchmark method

The comparison used the same NCNN CPU path for every model: ncnn `1.0.20261020`, two CPU threads, one inference pass, 128x128 input, and 512x512 output. TTA, tiling, face restoration, Ultra detail synthesis, Light AI, and JPEG post-processing were intentionally disabled so the backbone differences were isolated.

The test set covered:

- beach and compressed beach photograph;
- tree branches, wolf, and a brick building / architecture photograph;
- Lena portrait, children, and an additional anime face for face/skin behavior;
- fruit texture;
- rendered text with fine strokes, numerals, and a thin rectangle.

The PSNR/MAE numbers below compare each output with a bicubic enlargement of the original high-resolution reference. They are a stability/fidelity indicator, not ground-truth SR scores, because the benchmark inputs were deliberately reduced before inference. Visual inspection was used for the final choice, with special attention to skin, fine text, smooth skies, foliage, halos, ringing, chromatic edge noise, and crunchy texture.

## Results

Averages over the seven first-pass inputs (beach, building, children, wolf, trees, text, compressed beach):

| Candidate | Avg. PSNR to bicubic (dB) | Avg. MAE | High-frequency energy | Avg. one-pass CPU time | Qualitative result |
| --- | ---: | ---: | ---: | ---: | --- |
| Real-ESRGAN x4plus (baseline) | 23.045 | 10.551 | 11.243 | 2.369 s | Natural and safe, but visibly softer on masonry, foliage, and small text. |
| UltraSharp 4x | 24.449 | 8.229 | 10.653 | 2.414 s | Strong detail, but more cloud/branch chromatic ringing and edge overshoot. |
| Remacri 4x | 24.918 | 8.050 | 11.315 | 2.384 s | Sharp and capable, with a crunchier face/texture profile than LSDIR. |
| Ultramix Balanced 4x | 25.096 | 7.746 | 9.193 | 2.505 s | Good general balance, but less reliable fine text and architecture detail. |
| HFA2k 4x | 25.686 | 8.371 | 5.659 | 2.379 s | Very smooth and low-noise; intended for anime, and too soft for the photographic default. |
| **4xLSDIR** | **26.002** | **6.909** | 11.818 | 2.457 s | Best overall detail/fidelity balance; strongest building and text results without NMKD-style crunch. |
| LSDIRplusC 4x | 23.629 | 9.108 | 12.458 | 2.464 s | Compression-specialized; unnecessarily harsh on clean photographs. |
| Nomos8kSC 4x | 24.579 | 8.673 | 9.875 | 2.444 s | Natural and compression-tolerant, but softer and less legible on small text. |
| NMKD Siax 200k | 24.117 | 8.677 | 17.849 | 2.336 s | High apparent detail, but amplifies grain and produces a crunchy texture/halo profile. |
| NMKD Superscale SP | 24.269 | 8.331 | 15.618 | 2.338 s | Strong clean-image detail, but more texture exaggeration and less natural skin. |

The closest alternatives were also visually inspected on the Lena portrait and the architecture photograph. LSDIR retained clearer eyes, hat/fabric structure, masonry, and text than the baseline while avoiding the exaggerated skin/grain texture observed in NMKD Siax and the stronger ringing observed in UltraSharp. Its one-pass CPU cost was within the same range as the baseline; the existing TTA x8, tile sizing, Vulkan/CPU fallback, face identity safeguards, and post-processing pipeline are unchanged.

## Scope kept unchanged

- Offline Android and Windows packaging remains intact.
- TTA x8 still performs the same eight D4 transform passes and averages them.
- SCRFD + CodeFormer identity-safe face restoration remains separate from the SR backbone.
- Vulkan precision, tile-size, corrupted-output, integrated-GPU, and CPU fallback behavior remains unchanged.
- The old Real-ESRGAN pair remains in the package for an explicit conservative fallback choice and for compatibility diagnostics.
