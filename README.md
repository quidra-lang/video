# Quidra Video

Quidra Video is the first-party video package for Quidra. Core contains no
video decoder, FFmpeg wrapper, or video-specific runtime primitive.

`video.open(path)` snapshots the encoded file into an ordinary Quidra `bin`.
A `video.Reader` therefore has ordinary value semantics: copying a Reader
copies its logical position and preserves the exact opened resource even if the
original filesystem path is later moved, replaced, or damaged.

Decoded frames are CHW tensors. The element type is explicit at the package
boundary:

```quidra
import video

video.Reader reader = video.open("clip.mp4")
tensor<nat8> | none | error frame = reader.read<nat8>()
```

Use `channel = 1`, `3`, or `4` for gray, RGB, or RGBA. No CPU/GPU transfer
is implicit; decoding currently produces CPU tensors. FFmpeg integration lives
in `native/video_native.cpp` and accesses tensors only through
`<quidra/native_extension.h>`.

## Ownership rule

A primitive belongs to the package whose semantics it represents, not to Core
because Core could implement it faster. Video owns FFmpeg integration, decode
policy, pixel conversion, future SIMD/GPU kernels, and future codec-specific
optimizations. Core supplies only tensor/device/autograd/native-package
substrate.

## Errors

Video error values carry stable package-specific codes, while an error
raised by Core or Math and propagated with `try` retains its originating code.
The diagnostic messages above are unchanged:

| Code | Meaning | Typical cause |
| --- | --- | --- |
| `VIDEO_ARGUMENT` | Invalid caller argument | Negative or out-of-range seek, unsupported channel count |
| `VIDEO_DECODE` | Encoded media could not be decoded or probed | Invalid container or corrupt frame |
| `VIDEO_SHAPE` | Invalid decoded tensor dimensions | Non-positive frame width or height |

## Package metadata and release

`project.toml` is the metadata source of truth. Use `quidra package sync .`
to regenerate `quidra.package`; CI and releases verify it with
`quidra package validate .`. Video does not maintain a second TOML parser.

Development goes directly to `develop`. Published releases are immutable
`vMAJOR.MINOR.PATCH` tags created from tested `main`. Video uses the same
release version as Core and Math, so Core `vX.Y.Z` and Math `vX.Y.Z` must
exist before Video `vX.Y.Z`; the declared dependency ranges must admit that
shared version.

## License

MIT, matching Quidra Core.
