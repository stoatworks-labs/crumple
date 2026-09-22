#include "Shaders.h"

namespace crumple
{

const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

//---------------------------------------------------------------------------
// Pass 1: the sheet. Every facet of every layer drawn as a triangle, with
// additive blending -- the height is the SUM of the layers, so each pixel
// gets one flat facet's plane from each layer and they add. A facet's slope
// is a property of the whole facet, handed in flat; the height is interpolated
// across it, which for a plane is exact.
//---------------------------------------------------------------------------
const char* const kSheetVertexShader = R"(#version 410 core

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
)";

const char* const kSheetShader = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// Pass 2: the strain an inextensible sheet has to take up, 1/2 grad h grad h,
// at the picture's resolution. Mipmapped by the caller, so the grid below
// reads the AVERAGE over each of its cells, not a point sample of a quantity
// that jumps at every crease.
//---------------------------------------------------------------------------
const char* const kStrainShader = R"(#version 410 core

uniform sampler2D SheetTexture;

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec2 g    = texture( SheetTexture, uv ).yz;
	fragColor = vec4( 0.5 * g.x * g.x, 0.5 * g.x * g.y, 0.5 * g.y * g.y, 0.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 3: the strain on the solve's grid -- the frame mirrored to 2W x 2H --
// packed as two complex fields, ( Axx + i Ayy ) and ( Axy + i 0 ).
//---------------------------------------------------------------------------
const char* const kSourceShader = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// Passes 4 and 6: one Stockham radix-2 stage, along one axis, of TWO complex
// fields at once (.xy and .zw). Natural order in, natural order out, no bit
// reversal anywhere.
//
// Stage `Span` builds transforms of length Span out of pairs of length Span/2:
// output j takes its even half from j's own sub-transform and its odd half
// from the one Length/2 further on, twiddled by exp( +-2 pi i (j mod Span) /
// Span ). Millpond's, unchanged: written out for N = 4 in millpond's
// AGENTS.md and checked there against a double-precision DFT; `crtest --fft`
// re-checks this copy.
//---------------------------------------------------------------------------
const char* const kFFTShader = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// Pass 5: the least-squares stretch, per mode.
//
// Paper does not stretch: to second order in slope (Foppl-von Karman) its
// in-plane displacement u must make  sym grad u + A = 0  with A = 1/2 grad h
// grad h. That has an exact solution only where the sheet is developable, so
// solve it in least squares: minimise the integral of |sym grad u + A|^2. The
// Euler-Lagrange equation is  div( sym grad u + A ) = 0,  which per Fourier
// mode is the 2x2 system
//
//     1/2 ( |k|^2 I + k k^T ) u = r,    r = i A k,
//
// and its inverse is (2/|k|^2)( I - k k^T / (2|k|^2) ). Worked in AGENTS.md.
//---------------------------------------------------------------------------
const char* const kSolveShader = R"(#version 410 core

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
)";

//---------------------------------------------------------------------------
// Pass 7: composite. The print where the sheet pulled it, lit by the lamp.
//---------------------------------------------------------------------------
const char* const kCompositeShader = R"(#version 410 core

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
)";

} // namespace crumple
