# Pocket UI host library

`libpdui_host.a` compiles the production card decoder, page/image renderer,
GfxRenderer, font loader, shaping and decompression against host-only HALs.
There is no network, filesystem access, display driver or firmware upload path.
It builds independently of the GoogleTest project:

```sh
cmake -S host -B build/pdui-host -DCMAKE_BUILD_TYPE=Release
cmake --build build/pdui-host
```

Public C/Swift import surface: `include/PocketUIHost.h` and
`include/module.modulemap`. Link the archive, platform C++ runtime and zlib.
The version1 API currently renders **content cards and empty content only**.
It does not implement UI-pack application, home/settings/confirmation surfaces
or app UI. Apple packaging and provenance verification are described below.
The output is shared production rasterization, not physical-panel parity.

## Lifetime and failures

- Create validates panel geometry/orientation and copies at most64MiB of cpfont
  data. The caller can release its font input after the call returns.
- Each context owns one bounded framebuffer and its font resources. Destroy
  releases them. No allocations from this library ship on the ESP32.
- Serialize all operations on a context, including readback and destruction.
  Different contexts may be used on different threads, but rendering is
  internally serialized: production MiniBidi has static line scratch. The
  read-only asset HAL uses scoped thread-local bindings for font reopens.
- Supply the same font, orientation, metrics, localized/mapped labels and card
  as the device when comparing output. Labels are strict bounded UTF-8; the
  actual PDCT decoder validates card bytes. Image PBM is validated even if the
  current page has no remaining space to display it.
- Every render attempt invalidates old pixels, even an argument error. Only a
  fully successful render enables info/copy calls. Copy failures leave the
  caller's buffer unchanged. Geometry is physical, rows top-down, bits MSB-first,
  0 black/1 white. It is not a BMP and is not automatically orientation-rotated.
- No C++ exceptions cross the C ABI. A font-read failure is sticky inside the
  underlying loader; recreate the context after such a failure. Bad arguments,
  invalid documents and impossible layouts can be corrected and retried.

## Verification

Normal host CTest includes a C compiler/link check, synthetic cpfont lifecycle,
UTF-8/geometry refusals, frame invalidation and cross-thread independent-context
tests. The optional real-font checker compares24 C ABI page frames against
direct production calls, in addition to32 Cached/BoundedUI comparisons. See
`../test/gfx_host/README.md`.

On macOS, verify actual Swift import and execution without copying an artifact
into the app repository:

```sh
xcrun swiftc -I host/include test/gfx_host/HostAbiSwiftCheck.swift \
  build/pdui-host/libpdui_host.a -lc++ -lz -o build/host-abi-swift-check
build/host-abi-swift-check \
  firmware/sd-card/.fonts/PocketSansWorld/PocketSansWorld_12.cpfont
```

The cpfont is an existing local artifact. Neither command downloads a font or
contacts a reader. This is a macOS interop check, not an iOS artifact/build test.

## Apple artifact build

```sh
python3 scripts/test_build_host_apple.py
python3 host/build_apple.py
python3 host/build_apple.py --verify build/apple-host-<reported-id>
```

The builder uses installed Xcode SDKs and creates a fresh ignored output
directory on every run. It never overwrites an earlier artifact or copies to
the app. The result contains `PocketUIHost.xcframework` and `PROVENANCE.json`:

- macOS14: arm64 + x86_64;
- iOS17 device: arm64;
- iOS17 simulator: arm64 + x86_64.

Every archive's architectures are checked and an actual Swift consumer is
compiled/linked for each of the five slices. This is not execution on iOS or
acceptance of the companion app. The macOS consumer can be run with the local
cpfont using `<output>/macos/swift-link-arm64 <font>` on an Apple Silicon Mac.

Provenance schema1 stores the ABI version, commit, source file SHA-256 manifest
and aggregate digest, Xcode/compiler/CMake/SDK versions, minimum OS versions,
architectures and packaged file hashes. Source inventory includes uncommitted
and untracked code, HALs, public headers, build scripts and MiniBidi data. Local
compiler dependencies must all appear in that inventory. Source fingerprints
are compared before/after building; changes prevent sealing. Verification
rejects changed sources/commit, missing/wrong slices, changed/added files and
symlinks. This establishes local provenance/integrity, **not a digital signature
or trust/authentication boundary**. A failed build directory is left for diagnosis.

The app-side sync/import script must verify this record before accepting the
artifact and retain it alongside the XCFramework. No device firmware, source
tree or font is part of this package. App integration remains a separate step.
