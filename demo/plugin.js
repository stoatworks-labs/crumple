/**
 * Crumple — browser demo.
 *
 * The eight shaders below are copied unedited from `source/Shaders.cpp`: the
 * sheet (every facet of every fold generation, summed by additive blending),
 * the strain an inextensible sheet has to take up, the solve's source, the
 * Stockham FFT stage it shares with millpond, the per-mode least-squares
 * in-plane solve, and the composite with the lamp, the shadow march, the sheen
 * and the crease marks. `demo/tools/check_shaders.py` proves the text
 * character for character and `tools/verify.sh` runs it.
 *
 * What is a hand port, and is checked by nothing but a reader: `Controls.cpp`,
 * the whole of `Sheet.cpp` — the hashed junctions, the Bowyer-Watson
 * triangulation, the shared edges, the crease angles, `Formed` and
 * `FacetsFrom` — and the frame sequence of `CrumplePlugin::ProcessOpenGL`: the
 * vertex buffer, the dirty test, the grid choice, `ShadowSteps` and the lamp.
 * The parameter declarations come from the constructor in `Crumple.cpp`.
 *
 * **One word is removed from two shaders before they compile, and it is the
 * only thing on this page that touches the plugin's shader text.** The sheet
 * pass declares its barycentric varying `noperspective`. GLSL ES 3.00 has no
 * such qualifier — it arrived in ES 3.20 — so WebGL2 rejects the shader. The
 * word is taken out at load time by `withoutNoperspective()` below, and what
 * makes that honest rather than a rewrite is that it changes no arithmetic:
 * every sheet vertex is emitted with `gl_Position.w = 1.0`, and perspective-
 * correct interpolation divides by w, so with w = 1 it is the same linear
 * interpolation the qualifier asks for. The copies in this file stay verbatim
 * (that is what check_shaders.py compares); the removal is one named function
 * applied at `Program` construction, and the page says so.
 *
 * **Audio is absent.** Audio and Audio Scrunch read Resolume's FFT buffer; a
 * browser has none. They are left off the panel rather than shown dead, and
 * the removal is exact: with no spectrum the plugin's analyser reports a level
 * and a kick of 0, and Crumple is the slider alone.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, GLError, bindTexture } from './vendor/gl.js';

//===========================================================================
// The shaders. Copied from source/Shaders.cpp. Do not edit here.
//===========================================================================

const VERTEX = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
`;

const SHEET_VERTEX = `#version 410 core

layout( location = 0 ) in vec2 vCorner;  //frame-height units
layout( location = 1 ) in float vHeight;
layout( location = 2 ) in vec2 vSlope;   //the facet's dh/dx, dh/dy
layout( location = 3 ) in vec3 vCrease;  //each edge's crease angle, edge k opposite corner k
layout( location = 4 ) in vec3 vAltitude;//each corner's distance to its opposite edge

uniform float Aspect;

out float height;
flat out vec2 slope;
flat out vec3 crease;
flat out vec3 altitude;
noperspective out vec3 corner;

void main()
{
	gl_Position = vec4( 2.0 * vCorner / vec2( Aspect, 1.0 ) - 1.0, 0.0, 1.0 );
	height      = vHeight;
	slope       = vSlope;
	crease      = vCrease;
	altitude    = vAltitude;

	//Barycentric coordinates by hand: vertex k of each triangle is ( k == 0,
	//k == 1, k == 2 ), and interpolated across the facet they are how far
	//each point is from each edge, as a fraction of that corner's altitude.
	int k  = gl_VertexID % 3;
	corner = vec3( k == 0 ? 1.0 : 0.0, k == 1 ? 1.0 : 0.0, k == 2 ? 1.0 : 0.0 );
}
`;

const SHEET = `#version 410 core

uniform float MarkWidth;//the crease mark's width, frame-height units

in float height;
flat in vec2 slope;
flat in vec3 crease;
flat in vec3 altitude;
noperspective in vec3 corner;

out vec4 fragColor;

void main()
{
	//Where the fibres broke: a fine line along each edge, as strong as the
	//crease is sharp. Distance to edge k is corner[ k ] times altitude[ k ].
	vec3 away  = corner * altitude / MarkWidth;
	vec3 marks = crease * exp( -0.5 * away * away );

	fragColor = vec4( height, slope, marks.x + marks.y + marks.z );
}
`;

const STRAIN = `#version 410 core

uniform sampler2D SheetTexture;

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec2 g    = texture( SheetTexture, uv ).yz;
	fragColor = vec4( 0.5 * g.x * g.x, 0.5 * g.x * g.y, 0.5 * g.y * g.y, 0.0 );
}
`;

const SOURCE = `#version 410 core

uniform sampler2D StrainTexture;
uniform ivec2 GridSize;
uniform vec2 Domain;//2 Aspect, 2
uniform float Aspect;
uniform float Lod;  //the strain's mip level whose texel is one grid cell

out vec4 fragColor;

void main()
{
	ivec2 cell = ivec2( gl_FragCoord.xy );
	vec2 p     = ( vec2( cell ) + 0.5 ) * Domain / vec2( GridSize );

	//Fold into the frame. The mirrored sheet is even about both edges, so
	//the displacement across each edge is odd: the sheet's edges are held to
	//the frame's, which is what taping it down would do.
	if( p.x > Aspect )
		p.x = 2.0 * Aspect - p.x;
	if( p.y > 1.0 )
		p.y = 2.0 - p.y;

	vec4 A    = textureLod( StrainTexture, p / vec2( Aspect, 1.0 ), Lod );
	fragColor = vec4( A.x, A.z, A.y, 0.0 );
}
`;

const FFT = `#version 410 core

uniform sampler2D Source;
uniform sampler2D Twiddles;//exp( -2 pi i m / Length ), m = 0 .. Length-1, from doubles
uniform int Length;      //points along this axis
uniform int Span;        //the sub-transform length this stage produces: 2, 4 .. Length
uniform int Horizontal;  //1: along x
uniform float Direction; //-1 forward, +1 inverse

out vec4 fragColor;

vec2 times( vec2 a, vec2 b )
{
	return vec2( a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x );
}

void main()
{
	ivec2 cell = ivec2( gl_FragCoord.xy );
	int j      = Horizontal != 0 ? cell.x : cell.y;

	int halfSpan  = Span / 2;
	int evenIndex = ( j / Span ) * halfSpan + ( j % halfSpan );
	int oddIndex  = evenIndex + Length / 2;

	ivec2 evenCell = cell;
	ivec2 oddCell  = cell;
	if( Horizontal != 0 )
	{
		evenCell.x = evenIndex;
		oddCell.x  = oddIndex;
	}
	else
	{
		evenCell.y = evenIndex;
		oddCell.y  = oddIndex;
	}

	vec4 even = texelFetch( Source, evenCell, 0 );
	vec4 odd  = texelFetch( Source, oddCell, 0 );

	//The twiddle, from a table the CPU filled in double precision. Computing
	//it here with cos and sin costs a few units in the last place, and the
	//SAME few units every frame -- a systematic error, so it accumulates
	//coherently in a state that is fed back sixty times a second. Measured
	//on the walled pond: 7e-4 of mirror asymmetry after ten seconds with
	//cos and sin, against the table's figure in AGENTS.md.
	vec2 twiddle = texelFetch( Twiddles, ivec2( ( j % Span ) * ( Length / Span ), 0 ), 0 ).xy;
	if( Direction > 0.0 )
		twiddle.y = -twiddle.y;

	fragColor = even + vec4( times( odd.xy, twiddle ), times( odd.zw, twiddle ) );
}
`;

const SOLVE = `#version 410 core

uniform sampler2D Spectrum;//FFT of ( Axx + i Ayy, Axy + i 0 )
uniform ivec2 GridSize;
uniform vec2 Domain;
uniform float Norm;       //1 / ( Nx Ny ), the inverse transform's scale

out vec4 fragColor;

vec2 timesI( vec2 z )
{
	return vec2( -z.y, z.x );
}

void main()
{
	ivec2 cell = ivec2( gl_FragCoord.xy );

	//The mean is a rigid shift (pinned to none), and Nyquist cannot carry a
	//real field's derivative. Both carry nothing.
	if( ( cell.x == 0 && cell.y == 0 ) || cell.x == GridSize.x / 2 || cell.y == GridSize.y / 2 )
	{
		fragColor = vec4( 0.0 );
		return;
	}

	ivec2 opposite = ivec2( ( GridSize.x - cell.x ) % GridSize.x, ( GridSize.y - cell.y ) % GridSize.y );
	vec4 z  = texelFetch( Spectrum, cell, 0 );
	vec4 zm = texelFetch( Spectrum, opposite, 0 );
	zm.yw   = -zm.yw;

	//Unpack: ( Z + conj Z(-k) ) / 2 is the real field's, ( Z - conj Z(-k) ) / 2i
	//the imaginary one's.
	vec2 Axx  = 0.5 * ( z.xy + zm.xy );
	vec2 diff = 0.5 * ( z.xy - zm.xy );
	vec2 Ayy  = vec2( diff.y, -diff.x );
	vec2 Axy  = 0.5 * ( z.zw + zm.zw );

	ivec2 m = ivec2( cell.x < GridSize.x / 2 ? cell.x : cell.x - GridSize.x,
	                 cell.y < GridSize.y / 2 ? cell.y : cell.y - GridSize.y );
	vec2 k   = 6.28318530717958648 * vec2( m ) / Domain;
	float k2 = dot( k, k );

	vec2 rx = timesI( Axx * k.x + Axy * k.y );
	vec2 ry = timesI( Axy * k.x + Ayy * k.y );
	vec2 kr = k.x * rx + k.y * ry;

	vec2 ux = ( 2.0 / k2 ) * ( rx - k.x * kr / ( 2.0 * k2 ) );
	vec2 uy = ( 2.0 / k2 ) * ( ry - k.y * kr / ( 2.0 * k2 ) );

	//Both are real fields' spectra: pack them as ux + i uy for the way back.
	fragColor = Norm * vec4( ux.x - uy.y, ux.y + uy.x, 0.0, 0.0 );
}
`;

const COMPOSITE = `#version 410 core

uniform sampler2D InputTexture;
uniform vec2 MaxUV;
uniform vec2 HalfTexel;

uniform sampler2D SheetTexture;//h, h_x, h_y, marks
uniform sampler2D StretchTexture;//u_x, u_y on the solve's grid
uniform vec2 FrameToGrid;      //frame 0..1 to the grid's texture coordinates
uniform float Aspect;
uniform float Stretch;

uniform vec3 Lamp;        //unit, towards the lamp
uniform float Ambient;
uniform float Shadows;
uniform float ShadowReach;//frame-height units to march
uniform int ShadowSteps;  //about one every pixel and a half, 16 to 96
uniform float Sheen;
uniform float Wear;
uniform vec3 Paper;

uniform int View;
uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

vec4 picture( vec2 at )
{
	return texture( InputTexture, clamp( at, HalfTexel, vec2( 1.0 ) - HalfTexel ) * MaxUV );
}

float lambert( vec3 normal )
{
	return Ambient + ( 1.0 - Ambient ) * max( dot( normal, Lamp ), 0.0 );
}

float blinn( vec3 normal )
{
	vec3 halfway = normalize( Lamp + vec3( 0.0, 0.0, 1.0 ) );
	return pow( max( dot( normal, halfway ), 0.0 ), 48.0 );
}

void main()
{
	vec2 p       = uv * vec2( Aspect, 1.0 );
	vec4 sheet   = texture( SheetTexture, uv );
	vec3 normal  = normalize( vec3( -sheet.y, -sheet.z, 1.0 ) );
	vec3 level   = vec3( 0.0, 0.0, 1.0 );

	//The print: the material point that the sheet has pulled to here.
	vec2 u       = Stretch * texture( StretchTexture, uv * FrameToGrid ).xy;
	vec4 original = picture( uv );
	vec4 print    = picture( ( p - u ) / vec2( Aspect, 1.0 ) );

	//Shadow: march towards the lamp over the sheet and see whether it rises
	//above the ray. Soft over a hair's height, so a ridge's shadow does not
	//alias into stairs.
	float shade = 0.0;
	if( Shadows > 0.0 && Lamp.z < 0.9998 )
	{
		vec2 towards = normalize( Lamp.xy );
		float rise   = Lamp.z / length( Lamp.xy );
		for( int i = 1; i <= ShadowSteps; ++i )
		{
			float t  = ShadowReach * float( i ) / float( ShadowSteps );
			float hs = texture( SheetTexture, ( p + towards * t ) / vec2( Aspect, 1.0 ) ).x;
			shade    = max( shade, clamp( ( hs - ( sheet.x + rise * t ) ) / 0.0008, 0.0, 1.0 ) );
		}
	}

	//Lambert, normalised so a flat sheet under this lamp is exactly the
	//picture: the effect adds the creases' light and nothing else.
	float lit   = ( Ambient + ( 1.0 - Ambient ) * max( dot( normal, Lamp ), 0.0 ) * ( 1.0 - Shadows * shade ) )
	              / lambert( level );
	float gloss = Sheen * ( blinn( normal ) - blinn( level ) ) * ( 1.0 - Shadows * shade );

	vec3 colour = print.rgb * lit + vec3( gloss ) * print.a;
	//The crease marks show the paper's own colour through the ink.
	colour = mix( colour, Paper * lit * print.a, clamp( Wear * sheet.w, 0.0, 1.0 ) );

	vec4 result = vec4( colour, print.a );
	if( View == 1 )
		result = vec4( vec3( 0.5 + 4.0 * sheet.x ), 1.0 );
	else if( View == 2 )
		result = vec4( 0.5 + 0.5 * normal, 1.0 );
	else if( View == 3 )
		result = vec4( 0.5 + 25.0 * u, 0.5, 1.0 );

	fragColor = mix( original, result, MixAmount );
}
`;

/// The one change to the plugin's text: see the note at the top. Applied to
/// the two sheet stages only, and only to the qualifier.
const withoutNoperspective = (source) => source.replace(/\bnoperspective\s+/g, '');

//===========================================================================
// Controls.cpp, ported.
//===========================================================================

const f32 = Math.fround;
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const geometric = (v, lo, hi) => f32(lo * Math.pow(hi / lo, clamp(v, 0, 1)));
const linear = (v, lo, hi) => f32(lo + (hi - lo) * clamp(v, 0, 1));

const crumpleFromParam = (v) => f32(clamp(v, 0, 1));
const flattenFromParam = (v) => f32(clamp(v, 0, 1));
const scaleFromParam = (v) => geometric(v, 0.08, 0.6);
const reliefFromParam = (v) => linear(v, 0.0, 2.0);
const seedFromParam = (v) => clamp(Math.floor(f32(clamp(v, 0, 1) * f32(99.999))), 0, 99);
const stretchFromParam = (v) => linear(v, 0.0, 2.0);
const lampElevationFromParam = (v) => linear(v, 5.0, 90.0);
const lampAzimuthFromParam = (v) => linear(v, 0.0, 360.0);
const sheenFromParam = (v) => linear(v, 0.0, 0.6);

const K_MAX_LAYERS = 4;
const DETAIL_CELLS = [256, 512, 1024];
const K_PI = Math.PI;

//===========================================================================
// Sheet.cpp, ported.
//===========================================================================

const K_LAYER_RATIO = f32(0.45);
const K_STEEPNESS = f32(0.55);
const K_MAX_JUNCTIONS = 1600;
const K_FORMING_SPAN = f32(0.15);
const K_MARGIN = f32(0.02);

function lowbias32(x) {
  x >>>= 0;
  x ^= x >>> 16;
  x = Math.imul(x, 0x7feb352d) >>> 0;
  x ^= x >>> 15;
  x = Math.imul(x, 0x846ca68b) >>> 0;
  x ^= x >>> 16;
  return x >>> 0;
}

/// A uniform number in [0, 1) for ( seed, layer, index, which ). Integer only,
/// with Math.imul so the 32-bit wraparound is the C++'s.
function draw(seed, layer, index, which) {
  let h = lowbias32((Math.imul(seed, 0x9e3779b9) + 0x632be5ab) >>> 0);
  h = lowbias32((h ^ Math.imul(layer, 0x85ebca6b)) >>> 0);
  h = lowbias32((h ^ Math.imul(index, 0xc2b2ae35)) >>> 0);
  h = lowbias32((h ^ ((which + 0x27d4eb2f) >>> 0)) >>> 0);
  return h / 4294967296;
}

function buildJunctions(s) {
  const layers = [];
  const count = clamp(s.layers, 1, 4);
  for (let layer = 0; layer < count; layer += 1) {
    const junctions = [];
    let spacing = f32(s.scale * f32(Math.pow(K_LAYER_RATIO, layer)));
    let capped = false;
    for (let pass = 0; pass < 8; pass += 1) {
      const reach = f32(spacing + K_MARGIN);
      const area = f32(f32(s.aspect + 2 * reach) * f32(1 + 2 * reach));
      if (area / (spacing * spacing) <= K_MAX_JUNCTIONS * f32(0.98)) break;
      spacing = f32(Math.sqrt(area / (K_MAX_JUNCTIONS * f32(0.98))));
      capped = true;
    }

    const reach = f32(spacing + K_MARGIN);
    const left = -reach;
    const right = f32(s.aspect + reach);
    const below = -reach;
    const above = f32(1 + reach);
    const columns = Math.max(2, Math.ceil(f32(right - left) / spacing));
    const rows = Math.max(2, Math.ceil(f32(above - below) / spacing));
    const start = count > 1 ? f32(0.55 * layer / (count - 1)) : 0;

    for (let j = 0; j < rows; j += 1) {
      for (let i = 0; i < columns; i += 1) {
        const index = j * columns + i;
        junctions.push({
          x: f32(left + f32(f32(f32(i + f32(draw(s.seed, layer, index, 0))) * f32(right - left)) / columns)),
          y: f32(below + f32(f32(f32(j + f32(draw(s.seed, layer, index, 1))) * f32(above - below)) / rows)),
          height: f32((2 * draw(s.seed, layer, index, 2) - 1) * K_STEEPNESS * spacing),
          arrival: f32(start + 0.3 * f32(draw(s.seed, layer, index, 3))),
        });
      }
    }
    layers.push(junctions);
    if (capped) break;
  }
  return layers;
}

function formed(j, crumple) {
  const x = clamp(f32((crumple - j.arrival) / K_FORMING_SPAN), 0, 1);
  return f32(x * x * (3 - 2 * x));
}

function circumcircle(px, py, t) {
  const ax = px[t.a]; const ay = py[t.a];
  const bx = px[t.b]; const by = py[t.b];
  const cx = px[t.c]; const cy = py[t.c];
  const d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
  if (Math.abs(d) < 1e-18) return false;
  const a2 = ax * ax + ay * ay; const b2 = bx * bx + by * by; const c2 = cx * cx + cy * cy;
  t.cx = (a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / d;
  t.cy = (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / d;
  t.r2 = (ax - t.cx) * (ax - t.cx) + (ay - t.cy) * (ay - t.cy);
  return true;
}

/// Bowyer-Watson, in double, as the plugin does it.
function triangulate(points) {
  const n = points.length;
  const px = new Float64Array(n + 3);
  const py = new Float64Array(n + 3);
  const lo = [1e30, 1e30];
  const hi = [-1e30, -1e30];
  for (let i = 0; i < n; i += 1) {
    px[i] = points[i].x;
    py[i] = points[i].y;
    lo[0] = Math.min(lo[0], px[i]); lo[1] = Math.min(lo[1], py[i]);
    hi[0] = Math.max(hi[0], px[i]); hi[1] = Math.max(hi[1], py[i]);
  }
  const span = Math.max(hi[0] - lo[0], hi[1] - lo[1]) + 1;
  const mx = 0.5 * (lo[0] + hi[0]); const my = 0.5 * (lo[1] + hi[1]);
  px[n] = mx - 20 * span; py[n] = my - span;
  px[n + 1] = mx; py[n + 1] = my + 20 * span;
  px[n + 2] = mx + 20 * span; py[n + 2] = my - span;

  let triangles = [];
  const superTriangle = { a: n, b: n + 1, c: n + 2, cx: 0, cy: 0, r2: 0 };
  circumcircle(px, py, superTriangle);
  triangles.push(superTriangle);

  for (let p = 0; p < n; p += 1) {
    const boundary = [];
    const kept = [];
    for (const t of triangles) {
      const dx = px[p] - t.cx; const dy = py[p] - t.cy;
      if (dx * dx + dy * dy < t.r2) {
        for (const e of [[t.a, t.b], [t.b, t.c], [t.c, t.a]]) {
          const twin = boundary.findIndex((o) => o[0] === e[1] && o[1] === e[0]);
          if (twin >= 0) boundary.splice(twin, 1);
          else boundary.push(e);
        }
      } else {
        kept.push(t);
      }
    }
    for (const e of boundary) {
      const t = { a: e[0], b: e[1], c: p, cx: 0, cy: 0, r2: 0 };
      if (circumcircle(px, py, t)) kept.push(t);
    }
    triangles = kept;
  }

  const out = [];
  for (const t of triangles) {
    if (t.a >= n || t.b >= n || t.c >= n) continue;
    const cross = (px[t.b] - px[t.a]) * (py[t.c] - py[t.a]) - (py[t.b] - py[t.a]) * (px[t.c] - px[t.a]);
    out.push(t.a, cross > 0 ? t.b : t.c, cross > 0 ? t.c : t.b);
  }
  return out;
}

/// Each interior edge once, with the two facets that share it. The plugin
/// walks a std::map keyed on the ordered pair, so the list comes out in key
/// order; that is reproduced, although the creases do not depend on it.
function findShared(corners) {
  const edges = new Map();
  for (let t = 0; t < corners.length / 3; t += 1) {
    for (let k = 0; k < 3; k += 1) {
      const a = corners[t * 3 + (k + 1) % 3];
      const b = corners[t * 3 + (k + 2) % 3];
      const key = `${Math.min(a, b)},${Math.max(a, b)}`;
      if (!edges.has(key)) edges.set(key, []);
      edges.get(key).push([t, k]);
    }
  }
  const shared = [];
  for (const list of edges.values()) {
    if (list.length === 2) shared.push({ facetA: list[0][0], edgeA: list[0][1], facetB: list[1][0], edgeB: list[1][1] });
  }
  return shared;
}

function normalOf(f) {
  const ux = f.x[1] - f.x[0]; const uy = f.y[1] - f.y[0]; const uz = f.z[1] - f.z[0];
  const vx = f.x[2] - f.x[0]; const vy = f.y[2] - f.y[0]; const vz = f.z[2] - f.z[0];
  const n = [uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx];
  const l = Math.max(Math.hypot(n[0], n[1], n[2]), 1e-30);
  return n.map((c) => c / l);
}

function measureCreases(facets, shared) {
  for (const e of shared) {
    const n0 = normalOf(facets[e.facetA]);
    const n1 = normalOf(facets[e.facetB]);
    const angle = f32(Math.acos(clamp(n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2], -1, 1)));
    facets[e.facetA].crease[e.edgeA] = angle;
    facets[e.facetB].crease[e.edgeB] = angle;
  }
}

function buildLayers(s) {
  return buildJunctions(s).map((junctions) => {
    const corners = triangulate(junctions);
    return { junctions, corners, shared: findShared(corners) };
  });
}

function facetsFrom(layers, s) {
  const sheet = [];
  const flatten = clamp(s.flatten, 0, 1);
  layers.forEach((layer, layerIndex) => {
    const count = layer.corners.length / 3;
    const crumpled = [];
    for (let t = 0; t < count; t += 1) {
      const f = { x: [0, 0, 0], y: [0, 0, 0], z: [0, 0, 0], crease: [0, 0, 0], layer: layerIndex };
      for (let k = 0; k < 3; k += 1) {
        const j = layer.junctions[layer.corners[t * 3 + k]];
        f.x[k] = j.x;
        f.y[k] = j.y;
        f.z[k] = f32(f32(j.height * formed(j, s.crumple)) * s.relief);
      }
      crumpled.push(f);
    }
    // Creases are measured on the sheet as crumpled, BEFORE it is flattened.
    measureCreases(crumpled, layer.shared);
    for (const f of crumpled) f.z = f.z.map((z) => f32(z * f32(1 - flatten)));
    sheet.push(...crumpled);
  });
  return sheet;
}

//===========================================================================
// Crumple.cpp's helpers, ported.
//===========================================================================

function shadowSteps(reach, pictureHeight) {
  const lines = Math.min(pictureHeight, 1080);
  return clamp(Math.ceil(f32(reach * lines / 1.5)), 16, 96);
}

function chooseGrid(aspect, longCells) {
  const wide = aspect >= 1;
  const ratio = wide ? 1 / aspect : aspect;
  const shortCells = Math.max(8, Math.round(Math.pow(2, Math.round(Math.log2(longCells * ratio)))));
  return wide ? [longCells, shortCells] : [shortCells, longCells];
}

/// One vertex: corner (2), height, slope (2), creases (3), altitudes (3).
const VERTEX_FLOATS = 11;

//===========================================================================
// The frame, in the order ProcessOpenGL runs it.
//===========================================================================

class CrumpleRenderer {
  constructor(gl, quad) {
    this.gl = gl;
    this.quad = quad;

    // The sheet and the stretch are RGBA32F read LINEAR, and the strain is an
    // RGBA32F mip chain. Without OES_texture_float_linear WebGL2 treats all
    // three as incomplete and they sample as black: a flat, unlit, unstretched
    // print, which reads as "the effect does very little" rather than as an
    // error. So its absence stops the page.
    if (!gl.getExtension('OES_texture_float_linear')) {
      throw new GLError('OES_texture_float_linear is missing. The sheet, its strain mip chain and the stretch are 32-bit float textures read with a linear filter, as in the plugin; without the extension they would sample as black and the page would show an uncrumpled print instead of an error.');
    }

    this.sheetProgram = new Program(gl, withoutNoperspective(SHEET_VERTEX), withoutNoperspective(SHEET), 'sheet', {
      attribs: { vCorner: 0, vHeight: 1, vSlope: 2, vCrease: 3, vAltitude: 4 },
    });
    this.strainProgram = new Program(gl, VERTEX, STRAIN, 'strain');
    this.sourceProgram = new Program(gl, VERTEX, SOURCE, 'source');
    this.fft = new Program(gl, VERTEX, FFT, 'fft');
    this.solve = new Program(gl, VERTEX, SOLVE, 'solve');
    this.composite = new Program(gl, VERTEX, COMPOSITE, 'composite');

    this.sheet = new PassBuffer(gl, { filter: 'linear' });
    this.coarse = new PassBuffer(gl, { filter: 'nearest' });
    this.strain = new PassBuffer(gl, { filter: 'linear', mip: true });
    this.spectrum = [new PassBuffer(gl, { filter: 'nearest' }), new PassBuffer(gl, { filter: 'nearest' })];
    this.stretch = [new PassBuffer(gl, { filter: 'linear' }), new PassBuffer(gl, { filter: 'linear' })];
    this.stretchIndex = 0;
    this.gridWidth = 0;
    this.gridHeight = 0;
    this.twiddleX = null;
    this.twiddleY = null;

    this.vao = gl.createVertexArray();
    this.vbo = gl.createBuffer();
    gl.bindVertexArray(this.vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    const stride = VERTEX_FLOATS * 4;
    const attribute = (location, size, offsetFloats) => {
      gl.enableVertexAttribArray(location);
      gl.vertexAttribPointer(location, size, gl.FLOAT, false, stride, offsetFloats * 4);
    };
    attribute(0, 2, 0);
    attribute(1, 1, 2);
    attribute(2, 2, 3);
    attribute(3, 3, 5);
    attribute(4, 3, 8);
    gl.bindVertexArray(null);
    gl.bindBuffer(gl.ARRAY_BUFFER, null);
    this.vertexCount = 0;

    this.layers = null;
    this.layersFor = null;
    this.builtFor = null;
    this.relief = 0;
  }

  makeTwiddles(length) {
    const gl = this.gl;
    const table = new Float32Array(length * 2);
    for (let m = 0; m < length; m += 1) {
      const angle = -2 * K_PI * m / length;
      table[m * 2] = Math.cos(angle);
      table[m * 2 + 1] = Math.sin(angle);
    }
    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RG32F, length, 1, 0, gl.RG, gl.FLOAT, table);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.bindTexture(gl.TEXTURE_2D, null);
    return texture;
  }

  ensureBuffers(width, height, nx, ny) {
    const gl = this.gl;
    if (nx !== this.gridWidth || ny !== this.gridHeight) {
      if (this.twiddleX) gl.deleteTexture(this.twiddleX);
      if (this.twiddleY) gl.deleteTexture(this.twiddleY);
      this.twiddleX = this.makeTwiddles(nx);
      this.twiddleY = this.makeTwiddles(ny);
    }
    const coarseWidth = Math.min(width, 2 * nx);
    const coarseHeight = Math.min(height, 2 * ny);
    this.sheet.ensure(width, height, gl.RGBA32F);
    this.coarse.ensure(coarseWidth, coarseHeight, gl.RGBA32F);
    this.strain.ensure(coarseWidth, coarseHeight, gl.RGBA32F);
    for (const b of this.spectrum) b.ensure(nx, ny, gl.RGBA32F);
    for (const b of this.stretch) {
      b.ensure(nx, ny, gl.RGBA32F);
      // The plugin allocates the stretch with Wrap::Repeat; the kit clamps.
      gl.bindTexture(gl.TEXTURE_2D, b.texture);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.REPEAT);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.REPEAT);
    }
    gl.bindTexture(gl.TEXTURE_2D, null);
    this.gridWidth = nx;
    this.gridHeight = ny;
  }

  drawSheet(target, aspect, markWidth) {
    const gl = this.gl;
    target.clearTo(0, 0, 0, 0);
    gl.enable(gl.BLEND);
    gl.blendEquation(gl.FUNC_ADD);
    gl.blendFunc(gl.ONE, gl.ONE);
    this.sheetProgram.use();
    this.sheetProgram.set('Aspect', aspect);
    this.sheetProgram.set('MarkWidth', markWidth);
    gl.bindVertexArray(this.vao);
    gl.drawArrays(gl.TRIANGLES, 0, this.vertexCount);
    gl.bindVertexArray(null);
    gl.disable(gl.BLEND);
  }

  setInts(program, name, a, b) {
    const loc = program.location(name);
    if (loc !== null) this.gl.uniform2i(loc, a, b);
  }

  transform(buffers, current, direction) {
    const gl = this.gl;
    this.fft.use();
    this.fft.setSampler('Source', 0);
    this.fft.setSampler('Twiddles', 1);
    this.fft.set('Direction', direction);
    const axes = [[1, this.gridWidth, this.twiddleX], [0, this.gridHeight, this.twiddleY]];
    for (const [horizontal, length, twiddles] of axes) {
      this.fft.setInt('Horizontal', horizontal);
      this.fft.setInt('Length', length);
      const stages = Math.round(Math.log2(length));
      for (let s = 1; s <= stages; s += 1) {
        buffers[1 - current].bind();
        bindTexture(gl, 0, buffers[current].texture);
        bindTexture(gl, 1, twiddles);
        this.fft.setInt('Span', 1 << s);
        this.quad.draw();
        current = 1 - current;
      }
    }
    return current;
  }

  /// Everything from the facets to the stretch. Runs only when the sheet,
  /// the raster or the grid moved: a sheet nobody is moving is built once.
  build(settings, width, height, nx, ny) {
    const gl = this.gl;
    const lf = this.layersFor;
    if (!this.layers || settings.seed !== lf.seed || settings.layers !== lf.layers
      || settings.scale !== lf.scale || settings.aspect !== lf.aspect) {
      this.layers = buildLayers(settings);
      this.layersFor = { ...settings };
    }
    const drawn = facetsFrom(this.layers, settings);

    const vertices = new Float32Array(drawn.length * 3 * VERTEX_FLOATS);
    let v = 0;
    const highest = new Array(K_MAX_LAYERS).fill(0);
    const lowest = new Array(K_MAX_LAYERS).fill(0);
    for (const f of drawn) {
      const ux = f.x[1] - f.x[0]; const uy = f.y[1] - f.y[0]; const uz = f.z[1] - f.z[0];
      const vx = f.x[2] - f.x[0]; const vy = f.y[2] - f.y[0]; const vz = f.z[2] - f.z[0];
      const det = ux * vy - uy * vx;
      if (Math.abs(det) < 1e-14) continue;
      const hx = (uz * vy - uy * vz) / det;
      const hy = (ux * vz - uz * vx) / det;
      const altitude = [0, 1, 2].map((k) => {
        const a = (k + 1) % 3; const b = (k + 2) % 3;
        const ex = f.x[b] - f.x[a]; const ey = f.y[b] - f.y[a];
        return Math.abs(det) / Math.max(Math.sqrt(ex * ex + ey * ey), 1e-12);
      });
      for (let k = 0; k < 3; k += 1) {
        vertices.set([f.x[k], f.y[k], f.z[k], hx, hy, ...f.crease, ...altitude], v);
        v += VERTEX_FLOATS;
        const layer = clamp(f.layer, 0, K_MAX_LAYERS - 1);
        highest[layer] = Math.max(highest[layer], f.z[k]);
        lowest[layer] = Math.min(lowest[layer], f.z[k]);
      }
    }
    // The layers add, so the sheet's range is the sum of theirs.
    this.relief = 0;
    for (let l = 0; l < K_MAX_LAYERS; l += 1) this.relief = f32(this.relief + (highest[l] - lowest[l]));

    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    gl.bufferData(gl.ARRAY_BUFFER, vertices.subarray(0, v), gl.STREAM_DRAW);
    gl.bindBuffer(gl.ARRAY_BUFFER, null);
    this.vertexCount = v / VERTEX_FLOATS;

    this.ensureBuffers(width, height, nx, ny);
    const aspect = settings.aspect;

    // 1. The sheet, at the picture's size and at four samples a grid cell.
    this.drawSheet(this.sheet, aspect, Math.max(0.0012, 0.7 / height));
    this.drawSheet(this.coarse, aspect, 0.0012);

    // 2. The strain, and its mip chain.
    this.strain.bind();
    this.strainProgram.use();
    bindTexture(gl, 0, this.coarse.texture);
    this.strainProgram.setSampler('SheetTexture', 0);
    this.quad.draw();
    this.strain.generateMipmap();

    // 3-6. The stretch.
    const domainX = 2 * aspect;
    const domainY = 2;
    this.spectrum[0].bind();
    this.sourceProgram.use();
    bindTexture(gl, 0, this.strain.texture);
    this.sourceProgram.setSampler('StrainTexture', 0);
    this.setInts(this.sourceProgram, 'GridSize', this.gridWidth, this.gridHeight);
    this.sourceProgram.set('Domain', domainX, domainY);
    this.sourceProgram.set('Aspect', aspect);
    const pixelsPerCell = this.strain.height * domainY / this.gridHeight;
    const maxMip = Math.floor(Math.log2(Math.max(this.strain.width, this.strain.height)));
    this.sourceProgram.set('Lod', clamp(Math.log2(Math.max(pixelsPerCell, 1)), 0, maxMip));
    this.quad.draw();

    const current = this.transform(this.spectrum, 0, -1.0);

    this.stretch[0].bind();
    this.solve.use();
    bindTexture(gl, 0, this.spectrum[current].texture);
    this.solve.setSampler('Spectrum', 0);
    this.setInts(this.solve, 'GridSize', this.gridWidth, this.gridHeight);
    this.solve.set('Domain', domainX, domainY);
    this.solve.set('Norm', 1 / (this.gridWidth * this.gridHeight));
    this.quad.draw();

    this.stretchIndex = this.transform(this.stretch, 0, 1.0);
  }

  render({ input, params, width, height }) {
    const gl = this.gl;
    const pictureWidth = input.width;
    const pictureHeight = input.height;
    gl.disable(gl.BLEND);

    const aspect = f32(pictureWidth / pictureHeight);
    // Audio Scrunch is absent: with no spectrum the plugin adds nothing here.
    const crumple = clamp(crumpleFromParam(params.get('crumple')), 0, 1);

    const settings = {
      seed: seedFromParam(params.get('seed')),
      layers: params.option('layers') + 1,
      scale: scaleFromParam(params.get('scale')),
      relief: reliefFromParam(params.get('relief')),
      crumple,
      flatten: flattenFromParam(params.get('flatten')),
      aspect,
    };
    const [nx, ny] = chooseGrid(aspect, DETAIL_CELLS[params.option('detail')]);

    const b = this.builtFor;
    const dirty = !b || settings.seed !== b.seed || settings.layers !== b.layers || settings.scale !== b.scale
      || settings.relief !== b.relief || settings.crumple !== b.crumple || settings.flatten !== b.flatten
      || settings.aspect !== b.aspect || pictureWidth !== b.width || pictureHeight !== b.height
      || nx !== this.gridWidth || ny !== this.gridHeight;
    if (dirty) {
      this.build(settings, pictureWidth, pictureHeight, nx, ny);
      this.builtFor = { ...settings, width: pictureWidth, height: pictureHeight };
    }

    //-----------------------------------------------------------------------
    // 7. Composite.
    //-----------------------------------------------------------------------
    const elevation = lampElevationFromParam(params.get('lampElevation')) * K_PI / 180;
    const azimuth = lampAzimuthFromParam(params.get('lampAzimuth')) * K_PI / 180;
    const lamp = [Math.cos(elevation) * Math.cos(azimuth), Math.cos(elevation) * Math.sin(azimuth), Math.sin(elevation)];
    const reach = Math.min(this.relief / Math.max(Math.tan(elevation), 0.05) + 0.002, 0.6);

    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, width, height);
    const c = this.composite;
    c.use();
    bindTexture(gl, 0, input.texture);
    bindTexture(gl, 1, this.sheet.texture);
    bindTexture(gl, 2, this.stretch[this.stretchIndex].texture);
    c.setSampler('InputTexture', 0);
    c.setSampler('SheetTexture', 1);
    c.setSampler('StretchTexture', 2);
    c.set('MaxUV', 1, 1);
    c.set('HalfTexel', 0.5 / pictureWidth, 0.5 / pictureHeight);
    c.set('FrameToGrid', 0.5, 0.5);
    c.set('Aspect', aspect);
    c.set('Stretch', stretchFromParam(params.get('stretch')));
    c.set('Lamp', lamp[0], lamp[1], lamp[2]);
    c.set('Ambient', clamp(params.get('ambient'), 0, 1));
    c.set('Shadows', clamp(params.get('shadows'), 0, 1));
    c.set('ShadowReach', reach);
    c.setInt('ShadowSteps', shadowSteps(reach, pictureHeight));
    c.set('Sheen', sheenFromParam(params.get('sheen')));
    c.set('Wear', clamp(params.get('wear'), 0, 1));
    c.set('Paper', params.get('paperR'), params.get('paperG'), params.get('paperB'));
    c.setInt('View', params.option('view'));
    c.set('MixAmount', params.get('mix'));
    this.quad.draw();

    for (let unit = 2; unit >= 0; unit -= 1) bindTexture(gl, unit, null);
  }
}

//===========================================================================
// The parameters, from the constructor in Crumple.cpp. Audio and Audio
// Scrunch are left out: see the note at the top.
//===========================================================================

const std = (id, name, def, group, extra = {}) => ({ id, name, type: 'standard', default: def, group, ...extra });
const pct = (v) => `${Math.round(v * 100)}%`;

const PARAMS = [
  std('crumple', 'Crumple', 0.85, 'Sheet', {
    display: pct,
    hint: 'How far through the crumpling: 0 is a flat sheet, 1 has every junction of every generation risen. The generations arrive in turn, big folds first. Raising it only ever raises heights towards their final value.' }),
  std('flatten', 'Flatten', 0.4, 'Sheet', {
    display: pct,
    hint: 'How far it has been smoothed back out: every slope times (1 − this). The crease marks stay, because smoothing a sheet out does not unbreak its fibres.' }),
  std('scale', 'Scale', 0.55, 'Sheet', {
    display: (v) => `${scaleFromParam(v).toFixed(3)} of height`,
    hint: 'The first generation’s junction spacing — the size of its facets — as a fraction of the frame height.' }),
  { id: 'layers', name: 'Layers', type: 'option', default: 2, group: 'Sheet', elements: ['1', '2', '3', '4'],
    hint: 'Fold generations, each 0.45 times finer than the one before. The height is their sum, which is still flat facets.' },
  std('relief', 'Relief', 0.35, 'Sheet', {
    display: (v) => `${reliefFromParam(v).toFixed(2)}×`,
    hint: 'A multiplier on every facet’s slope. 1 is the sheet as generated — facets tilted by up to about 30 degrees.' }),
  std('seed', 'Seed', 0.0, 'Sheet', {
    display: (v) => `#${seedFromParam(v)}`,
    hint: 'Which sheet, 0 to 99. The junctions come from an integer hash, so a seed is the same sheet on every machine.' }),
  std('stretch', 'Stretch', 0.5, 'Sheet', {
    display: (v) => `${stretchFromParam(v).toFixed(2)}${Math.abs(stretchFromParam(v) - 1) < 1e-3 ? ' (physical)' : ''}`,
    hint: 'How far the print follows the sheet. 1 is physical — the print pulled in by exactly what an inextensible sheet needs, from a least-squares Föppl–von Kármán solve on an FFT — and 0 leaves it where it was printed, which is what a texture overlay would do.' }),
  { id: 'detail', name: 'Detail', type: 'option', default: 1, group: 'Sheet', elements: ['256', '512', '1024'],
    hint: 'The stretch solve’s grid along the mirrored domain’s long side.' },

  std('lampElevation', 'Lamp Elevation', 0.41, 'Lamp', {
    display: (v) => `${lampElevationFromParam(v).toFixed(1)}°` }),
  std('lampAzimuth', 'Lamp Azimuth', 0.375, 'Lamp', {
    display: (v) => `${lampAzimuthFromParam(v).toFixed(0)}°`,
    hint: '0 is a lamp to the right; 135 is the top left.' }),
  std('ambient', 'Ambient', 0.4, 'Lamp', { display: pct }),
  std('shadows', 'Shadows', 0.8, 'Lamp', {
    display: pct,
    hint: 'A march towards the lamp over the sheet: where a ridge rises above the ray, the point is in its shadow.' }),
  std('sheen', 'Sheen', 0.25, 'Lamp', {
    display: (v) => sheenFromParam(v).toFixed(2),
    hint: 'A Blinn highlight’s strength, relative to a flat sheet — so a flat sheet under any lamp is exactly the picture.' }),
  std('wear', 'Wear', 0.4, 'Lamp', {
    display: pct,
    hint: 'The crease marks: where the fibres broke, the paper’s own colour shows through the ink, as strong as the crease was sharp.' }),
  { id: 'paperR', name: 'Paper', type: 'colour', default: 0.97, group: 'Lamp' },
  { id: 'paperG', name: 'Paper_Green', type: 'colour', default: 0.96, group: 'Lamp' },
  { id: 'paperB', name: 'Paper_Blue', type: 'colour', default: 0.93, group: 'Lamp' },

  { id: 'view', name: 'View', type: 'option', default: 0, group: 'Output', elements: ['Picture', 'Height', 'Normals', 'Stretch'],
    hint: 'Picture is the effect; the others show the sheet’s height, its normals, or the stretch field on their own.' },
  std('mix', 'Mix', 1.0, 'Output', { display: pct }),
];

mountDemo({
  name: 'Crumple',
  pluginId: 'CR01',
  tagline: 'Crumpled paper. The sheet is a sum of triangulated, piecewise-flat fold generations; the print is pulled in by the stretch an inextensible sheet needs, so it kinks at every crease; then a lamp, its shadows and the crease marks.',
  repo: 'https://github.com/stoatworks-labs/crumple',
  page: 'https://stoatworks-labs.com/software/crumple/',
  video: 'https://www.youtube.com/watch?v=wWfb8NyeoY8',

  // RGBA32F throughout, and the sheet sums its layers by blending into it.
  needFloat: true,
  needFloatBlend: true,

  // The geometry card first: straight lines are what show a print being
  // pulled in at a crease.
  sources: ['grid', 'bars', 'scene', 'ramp', 'detail', 'spot'],

  params: PARAMS,

  differences: [
    'The eight shaders are the plugin’s own text, and demo/tools/check_shaders.py proves it. The sheet generator — the hashed junctions, the Delaunay triangulation, the crease angles, how far each junction has risen — is a hand port of Sheet.cpp to JavaScript, and so are the control conversions and the frame sequence (the vertex buffer, the rebuild-only-when-something-moved test, the grid, the shadow reach and step count, the lamp). Nothing checks that port but a reader.',
    'One word is removed from the sheet pass before it compiles. Its barycentric varying is declared noperspective, which GLSL ES 3.00 does not have, so WebGL2 would reject the shader. Every sheet vertex has w = 1, and perspective-correct interpolation divides by w, so with the word gone the interpolation is the same arithmetic. It is the only change the page makes to the plugin’s shader text; the copy on the page is verbatim and the removal happens at load.',
    'The audio side is not here. The plugin can add the level of Resolume’s FFT buffer, and a kick, to Crumple through Audio Scrunch; a browser has no equivalent and asking for a microphone to demonstrate a video effect is not a trade worth making. Audio and Audio Scrunch are absent rather than present and dead, and the removal is exact: with no spectrum the plugin adds nothing to Crumple either.',
    'Float render targets and filtering. The sheet, the strain, the spectrum and the stretch are 32-bit float targets (EXT_color_buffer_float), the fold generations sum by blending into one (EXT_float_blend), and the sheet, the strain’s mip chain and the stretch are read with a linear filter (OES_texture_float_linear). All three are core in the plugin’s GL 4.1 and optional in WebGL2; the page stops with a message if one is missing rather than show an uncrumpled print.',
    'The About block is not here; the links are in the header.',
  ],

  createRenderer: (gl, quad) => new CrumpleRenderer(gl, quad),
});
