# Attributions

crumple is built on other people's work. This file lists what that work is,
who did it, and what it is doing here.

> **Provisional.** Across the fleet this file is generated from master lists in
> `stoatworks-backend` by `scripts/sync-attributions.py`. crumple is not
> registered there yet, so this copy is hand-written. Register it before release
> — and note that the script's `--only` flag truncates the file rather than
> filtering it.

## Third-party code this project uses

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>
Licence: BSD-3-Clause
Copyright: FreeFrame

Vendored as a git submodule at `external/ffgl`, pinned to `b1afaf9`.

The plugin ABI itself. An FFGL effect is defined by this SDK's headers — there
is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Windows only, from vcpkg, statically linked. The SDK's headers pull it in for
the OpenGL function pointers; macOS uses the system OpenGL framework instead.

### zlib

<https://zlib.net>
Licence: zlib
Copyright: Jean-loup Gailly and Mark Adler

Ships with macOS. The offline harness links it to deflate its PNG output.
Nothing in the shipped plugin uses it.

## Work from elsewhere in the fleet

### millpond — the Fourier transform

<https://github.com/stoatworks-labs/millpond>
Licence: MIT
Copyright: Stoatworks Labs

The Stockham FFT shader, its double-precision twiddle tables and the
`Transform` loop are millpond's, unchanged. millpond checks them against a
double-precision DFT, and here `--isometry` exercises them end to end. Also
millpond's `GLState.h`, `PassBuffer` (with its wrap modes), `Diag`, harness
plumbing, `--pipe`/`--script`, `tools/sweep.py` and `tools/verify.sh`, which
millpond itself took from tinsel, intaglio and vectrix.

### rosette — the audio analyser and the clock

<https://github.com/stoatworks-labs/rosette>
Licence: MIT
Copyright: Stoatworks Labs

`source/Audio.{h,cpp}` (via millpond) and the host-clock unit vote.

## Method

Described in books and papers, not copied from anyone's source:

- Delaunay triangulation by the Bowyer–Watson algorithm.
- The Föppl–von Kármán in-plane strain of thin plates, ε = sym∇u + ½∇h⊗∇h,
  here minimised in least squares per Fourier mode.
- Lambert diffuse and Blinn–Phong specular shading; heightfield shadows by
  marching.
- Crumpled paper as flat facets meeting at straight creases and at junctions
  (the "d-cones" of the physics literature) — the picture this model is built
  to match.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or
you would rather not be listed — open an issue and it will be fixed.
