# crumple — for agents

The why behind the code. It was built on 2026-09-23 as the stretch goal of
Allan's request ("scrunched paper and other similar effects"), in the same
session as millpond. The spec is `~/Projects/resolume/specs/SPEC-crumple.md`.

## The one idea

Paper bends but does not stretch. Everything else follows:

- the sheet is facets and creases;
- the print is pulled in where the sheet tilts, and kinks where it creases;
- a lamp lights each facet at its own angle.

## The sheet, and why it is not the first model

**The first model was a sum of tent ridges.** Each crease was a finite ridge
with a rounded spine and a tapered end. It rendered as sticks lying on a flat
page: the space between ridges stayed flat, and crumpled paper has no flat
space. It was replaced within the hour. The lesson is general: a crumple is
*all* facets, so the model has to tile the sheet with tilted planes, not
decorate it.

**The model now is a sum of up to four piecewise-planar layers.** Each layer
is built like this:

- Junctions are stratified, one per cell of a grid at the layer's spacing, so
  facet sizes are even and nothing is a sliver.
- They are joined by a Delaunay triangulation (Bowyer–Watson in double; O(n²),
  cached).
- Each junction gets a height of ±`kSteepness`·spacing, so every generation
  has slopes of the same order.

The sum of piecewise-linear functions is piecewise linear on the overlay, so
every facet of the SUM is flat too.

- **Generations arrive in turn.** Layer L's junctions arrive between 0.55·L/(n−1)
  and that + 0.3, and each rises over 0.15. Everything is whole at Crumple 1,
  and a height only ever grows with Crumple.
- **Each layer reaches one cell plus 0.02 past the frame.** The corner cells'
  junctions then lie diagonally outside the frame's corners, and their convex
  hull (the triangulation's extent) covers the frame. `--monotone` samples
  6,000 points per layer to prove it.
- **The GPU draws facets, not creases.** Each triangle carries its facet's
  slope as a flat attribute (computed on the CPU from its three corners), its
  three edges' crease angles, and its three altitudes. Barycentrics come from
  `gl_VertexID % 3`, so the distance to each edge is barycentric × altitude,
  and that places the crease marks. Additive blending sums the layers.

## The stretch

To second order in slope, a sheet that does not stretch has in-plane
displacement u with ε(u) + A = 0, where ε = sym∇u and A = ½∇h⊗∇h. That is
solvable exactly only where the sheet is developable, so it is solved in least
squares: minimise ∫|ε + A|². The Euler–Lagrange equation is div(ε + A) = 0.
Per Fourier mode, with ∂ → ik:

    ½(|k|² û_b + k_b (k·û)) = i (Â k)_b

The matrix ½(|k|² I + kkᵀ) has inverse (2/|k|²)(I − kkᵀ/(2|k|²)). To check:
(|k|²I + kkᵀ)(I − kkᵀ/2|k|²) = |k|²I + kkᵀ(−½ + 1 − ½) = |k|²I. The mean
(a rigid shift) and Nyquist are zeroed. A's three components go through one
RGBA forward transform, packed as (Axx + iAyy, Axy + i0). The solve unpacks
them with the Hermitian trick and packs û_x + iû_y for one inverse.

The domain is the frame mirrored to 2W × 2H. A is even about both edges, so
u_x is odd in x and u_y is odd in y: the sheet's edges are held to the frame's
edges. The composite reads the print from x − u.

For a single straight ridge, A_xx = ½s² on the two facets and 0 elsewhere,
which is developable. The exact solution has du_x/dx = −A_xx + mean. Over an
interval spanning the ridge, the print pulls in by ½s²·2W more than over an
equal interval of flat sheet. That is `--isometry`, and it holds to 0.3% at
Detail 512 and 1024.

## The traps

**`flat` is a GLSL reserved word, and `round` is a built-in.** Both were in
the first composite, as the flat normal and the rounded spine.
`verify.sh`'s grep catches `flat`. Nothing catches `round` except the compiler.

**A picture-sized strain buffer made a moving sheet cost 13.7 ms at 4K.**
Drawing the sheet, squaring its gradient and building a mip chain of RGBA32F
at 3840×2160, every frame the sheet moves. The solve only ever reads the
strain averaged over its own grid cells, so the strain now comes from a
second, coarse drawing at four samples a cell (`coarse`): 2.9 ms. That coarse
drawing places facet edges to half a coarse pixel. At Detail 256 that is the
largest error `--isometry` sees (3.9e-4), and it is the bound's second term.

**A still sheet is rebuilt for nothing.** Most frames nothing that shapes the
sheet has moved. `ProcessOpenGL` keeps what it last built for, and a frame
whose settings, raster and grid all match is the composite alone (0.1–0.3 ms).
Anything new that shapes the sheet must be added to the `dirty` test, or it
will change nothing until something else does.

**The shadow march must be adaptive.** A fixed 40 steps put a ridge's peak up
to half a step between samples, 0.011 of the frame at the test's reach, and
shortened the shadow by s·step/(2 tan e). The step count is now
`ShadowSteps()`: one every 1.5 pixels of a picture of at most 1080 lines, 16
to 96 steps. The harness uses the same function to derive its bound.

**The `x − u` readback also sees the light.** Checking that the composite reads
the print from x − u needs every other term to be the identity, and Lambert is
not: Ambient 1 makes it so.

## Every numeric check, and where its tolerance comes from

| check | bound | why |
| --- | --- | --- |
| `--flat` | 1e-5 | every term is normalised to the flat sheet; measured 0 |
| `--isometry` pull | max(1% of the pull, 1.1 × ½s² × one coarse pixel) | facet edges placed to half a coarse pixel each side; measured 0.3% at the finer Details |
| `--isometry` sideways | 1e-4 of the pull per width | a vertical ridge has no y-strain; measured 1e-9 |
| `--isometry` x − u | 1e-5 | the coordinate card is linear, so reading it anywhere is exact to float |
| `--lambert` | 1e-5 | the formula; measured 1e-7 |
| `--shadow` | s·step/(2 tan e) + 1.5 px | a peak can fall between two march samples |
| `--monotone` | exact | integer hashing, and smoothstep arrivals |

Each has a negative control (one 8-bit step brighter, 5% more pull, the lamp
one degree higher, 15% longer shadows, the next seed's sheet), and all five
fail as they must.

## Decisions taken without asking

- **A separate repo from millpond.** It is a different mechanism: an opaque
  folded sheet under a lamp, not a transparent moving surface.
- **Small-slope isometry, in least squares.** The nonlinear version is a real
  simulation, and this one is exact for developable pieces and honest about
  the junctions.
- **The edges are pinned**, because mirroring makes the spectral solve clean.
  A real sheet's edges would pull in.
- **Lambert is normalised to the flat sheet**, so the effect adds the creases'
  light and nothing else. The cost is that facets facing the lamp go brighter
  than the picture and clip on white paper.
- **Defaults:** Crumple 0.85, Flatten 0.4, Relief 0.35 (0.7 of the sheet as
  generated), three generations, the lamp at 40° from the top left. It reads
  as a crumpled, part-smoothed page the moment it is dropped on a layer.

## What is verified, and what is assumed

Verified on this Mac (M4 Max, macOS 26.4.1):

- everything in the README's Status table;
- `tools/verify.sh` green;
- `oxbow probe` reads `SW Crumple` / `CR01` / effect, and `oxbow selftest`
  passes.

Assumed or not done:

- never loaded into Resolume;
- never built on Windows, where float blending of RGBA32F (used by the sheet
  draw) is standard on DX11-class GPUs but untested;
- Resolume's FFT bins are as rosette assumed them;
- the limits in the README's Status.
