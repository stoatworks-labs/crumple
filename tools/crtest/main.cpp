/**
    crtest -- render Crumple offline, and measure what its sheet is doing.

    It drives the REAL plugin class, through the same ProcessOpenGL a host
    calls. A test that exercises a reimplementation tests the reimplementation.

        crtest --out /tmp/frame.png     the card, crumpled
        crtest --card /tmp/card.png     the card on its own
        crtest --list                   every parameter and its default
        crtest --pipe / --film N        the fleet's raw frame format, with --script

    The claims:

        --flat        at Crumple 0 the output is the picture
        --isometry    across one straight ridge the print pulls in by exactly
                      1/2 s^2 of its width: paper does not stretch
        --lambert     each facet of a known crease is lit by Lambert, exactly
        --shadow      a ridge of height H under a lamp at elevation e casts a
                      shadow H / tan e long
        --monotone    raising Crumple only ever raises the sheet, and the
                      facets are a true triangulation of the frame
        --negative    every check above, against a wrong model, must fail
        --bench       time a frame at 720p through 4K

    Every length is a fraction of the frame height, so a slope is the same
    number at any raster; the checks that read pixels run at two rasters
    anyway.
*/

#include "Controls.h"
#include "Crumple.h"
#include "Sheet.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace crumple;

namespace
{
constexpr double kPi = 3.14159265358979323846;

using Floats = std::vector< float >;
using Bytes  = std::vector< unsigned char >;

int g_failures = 0;

std::string fmt( const char* format, ... )
{
	char buffer[ 1024 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	return buffer;
}

void Check( bool condition, const std::string& message )
{
	std::printf( "  %s  %s\n", condition ? "ok  " : "FAIL", message.c_str() );
	if( !condition )
		++g_failures;
}

int Verdict()
{
	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

/// Deliberate errors for --negative: each makes one check score the plugin
/// against a model wrong by an amount the check must be able to see.
struct Perturb
{
	double strainFactor = 1.0;  ///< --isometry expects this times 1/2 s^2
	double lampTilt     = 0.0;  ///< --lambert expects the lamp this many degrees higher
	double shadowFactor = 1.0;  ///< --shadow expects this times H / tan e
	double flatSlack    = 0.0;  ///< --flat adds this to the expected output
	bool reseeded       = false;///< --monotone scores against another seed's sheet
};

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( Bytes& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( Bytes& out, const char* type, const Bytes& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

/// `rgba` is floats, row 0 at the BOTTOM (GL's order); the file is written top
/// row first, which is the only place anything here flips.
bool writePng( const std::string& path, int width, int height, const Floats& rgba )
{
	Bytes raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		for( int x = 0; x < width; ++x )
			for( int c = 0; c < 4; ++c )
			{
				const float v = rgba[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ];
				raw.push_back( static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) ) );
			}
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	Bytes compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	Bytes png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	Bytes ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.insert( ihdr.end(), { 8, 6, 0, 0, 0 } );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Integer hashing, for the card. Never fract( sin( x ) * 43758.5453 ): that is
// the driver's answer, and two machines disagree about it.
//---------------------------------------------------------------------------
uint32_t lowbias32( uint32_t x )
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

double hash01( uint32_t a, uint32_t b = 0 )
{
	return static_cast< double >( lowbias32( a ^ lowbias32( b + 0x9e3779b9U ) ) ) / 4294967296.0;
}

//---------------------------------------------------------------------------
// The card: a printed page. A ruled grid, so the kink at every crease is
// plain to see; a photograph's worth of sky and sea; a headline and lines of
// "text" -- dashes of ink in rows. Rows are v = 0 first, as GL stores them.
//---------------------------------------------------------------------------
Floats buildCard( int width, int height )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	const double aspect = static_cast< double >( width ) / height;
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 ) / height, v = ( y + 0.5 ) / height;
			double r = 0.95, g = 0.93, b = 0.88;//paper
			bool blank = true;

			if( u > aspect * 0.55 && u < aspect * 0.95 && v > 0.45 && v < 0.92 )
			{
				//A photograph: sky into sea, and a sun.
				const double t = ( v - 0.45 ) / 0.47;
				r = 0.95 - 0.55 * t;
				g = 0.75 - 0.3 * t;
				b = 0.55 + 0.35 * t;
				const double su = u - aspect * 0.8, sv = v - 0.74;
				if( su * su + sv * sv < 0.004 )
				{
					r = 1.0;
					g = 0.85;
					b = 0.45;
				}
				if( v < 0.62 )
				{
					const double wave = 0.05 * std::sin( 90.0 * u + 12.0 * std::sin( 20.0 * v ) );
					r = 0.05 + wave;
					g = 0.22 + wave;
					b = 0.42 + wave;
				}
				blank = false;
			}
			else if( u > 0.06 && u < aspect * 0.5 )
			{
				if( v > 0.82 && v < 0.9 )
				{
					r = 0.75;
					g = 0.12;
					b = 0.1;
					blank = false;
				}
				const double line = std::fmod( v, 0.045 );
				if( v < 0.78 && v > 0.1 && line > 0.012 && line < 0.026 )
				{
					const int row   = static_cast< int >( v / 0.045 );
					const double at = u * 9.0 + 0.37 * row;
					if( std::fmod( at, 1.0 ) < 0.78
					    && hash01( static_cast< uint32_t >( row * 131 + static_cast< int >( at ) ) ) > 0.08 )
					{
						r = g = b = 0.12;
						blank = false;
					}
				}
			}
			//A ruled grid on the bare paper: the clearest witness to a kink.
			if( blank && ( std::fmod( u, 0.05 ) < 0.0025 || std::fmod( v, 0.05 ) < 0.0025 ) )
			{
				r = 0.55;
				g = 0.7;
				b = 0.85;
			}

			const size_t o = ( static_cast< size_t >( y ) * width + x ) * 4;
			card[ o + 0 ]  = static_cast< float >( r );
			card[ o + 1 ]  = static_cast< float >( g );
			card[ o + 2 ]  = static_cast< float >( b );
			card[ o + 3 ]  = 1.0f;
		}
	return card;
}

Floats flatCard( int width, int height, float value )
{
	Floats card( static_cast< size_t >( width ) * height * 4, value );
	for( size_t i = 3; i < card.size(); i += 4 )
		card[ i ] = 1.0f;
	return card;
}

/// R is the frame's x in frame-height units, G its y, at pixel centres: read
/// anywhere, it returns where it was read from.
Floats coordinateCard( int width, int height )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const size_t o = ( static_cast< size_t >( y ) * width + x ) * 4;
			card[ o + 0 ]  = static_cast< float >( ( x + 0.5 ) / height );
			card[ o + 1 ]  = static_cast< float >( ( y + 0.5 ) / height );
			card[ o + 2 ]  = 0.0f;
			card[ o + 3 ]  = 1.0f;
		}
	return card;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without
	//a GPU, where it will at least prove the shaders compile.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, const float* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

//---------------------------------------------------------------------------
// The audio the harness feeds, as millpond's feeds it: written into
// the Audio buffer's elements the way the host writes them.
//---------------------------------------------------------------------------
enum class AudioFeed
{
	Silence,
	Pulses///< a bass-heavy spectrum with a hit every half second
};

void feedAudio( CrumplePlugin& plugin, double seconds, AudioFeed feed )
{
	const double beat  = std::fmod( seconds, 0.5 );
	const float strike = feed == AudioFeed::Pulses ? static_cast< float >( 0.15 + 1.5 * std::exp( -beat / 0.06 ) ) : 0.0f;
	for( int bin = 0; bin < audio::kBins; ++bin )
	{
		const float across = static_cast< float >( bin ) / static_cast< float >( audio::kBins - 1 );
		const float shape  = 0.7f * ( 1.0f - across ) * ( 1.0f - across ) + 0.2f * ( 0.5f + 0.5f * std::sin( 25.0f * across ) );
		plugin.SetParamElementValue( PT_AUDIO, static_cast< unsigned int >( bin ), shape * strike );
	}
}

//---------------------------------------------------------------------------
// A rig: the real plugin, a float input and a float output, at one size.
//---------------------------------------------------------------------------
struct Rig
{
	CrumplePlugin plugin;
	int width = 0, height = 0;
	GLuint sourceTexture = 0, outputTexture = 0, outputFBO = 0;
	int frame            = 0;
	double fps           = 60.0;
	AudioFeed feed       = AudioFeed::Silence;

	ProcessOpenGLStruct process    = {};
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };

	~Rig()
	{
		plugin.DeInitGL();
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
	}

	bool Init( int w, int h, const Floats* picture = nullptr )
	{
		width  = w;
		height = h;
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		plugin.SetClockScaleForTest( 1.0 );

		const Floats card = picture ? *picture : buildCard( width, height );
		sourceTexture     = makeTexture( width, height, card.data() );
		outputTexture     = makeTexture( width, height, nullptr );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			return false;

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;
		process.numInputTextures                        = 1;
		process.inputTextures                           = inputs;
		process.HostFBO                                 = outputFBO;
		return true;
	}

	void Upload( const Floats& picture )
	{
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, picture.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void Set( unsigned int id, float value )
	{
		plugin.SetFloatParameter( id, value );
	}

	/// Only the lamp's Lambert term: no shadows, sheen, marks or stretch.
	void Bare()
	{
		Set( PT_SHADOWS, 0.0f );
		Set( PT_SHEEN, 0.0f );
		Set( PT_WEAR, 0.0f );
		Set( PT_STRETCH, 0.0f );
	}

	bool Render( int frames = 1 )
	{
		for( int i = 0; i < frames; ++i )
		{
			const double seconds = static_cast< double >( frame ) / fps;
			plugin.SetTime( seconds );
			feedAudio( plugin, seconds, feed );
			++frame;
			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "ProcessOpenGL failed\n" );
				return false;
			}
		}
		return true;
	}

	Floats Output() const
	{
		Floats pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}

	/// ( u_x, u_y, ., . ) per grid cell, row 0 first.
	Floats StretchField() const
	{
		Floats data( static_cast< size_t >( plugin.GridWidth() ) * plugin.GridHeight() * 4 );
		glBindTexture( GL_TEXTURE_2D, plugin.StretchTextureID() );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, data.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return data;
	}
};

/// u at frame point ( x, y ), frame-height units, by linear interpolation
/// between grid points at ( i + 1/2 ) cells over the mirrored domain.
double stretchAt( const Floats& field, int nx, int ny, double aspect, double x, double y, int channel )
{
	const double fx = x / ( 2.0 * aspect / nx ) - 0.5;
	const double fy = y / ( 2.0 / ny ) - 0.5;
	const int i = static_cast< int >( std::floor( fx ) ), j = static_cast< int >( std::floor( fy ) );
	const double tx = fx - i, ty = fy - j;
	auto at = [ & ]( int a, int b ) {
		a = ( ( a % nx ) + nx ) % nx;
		b = ( ( b % ny ) + ny ) % ny;
		return static_cast< double >( field[ ( static_cast< size_t >( b ) * nx + a ) * 4 + channel ] );
	};
	return ( 1 - ty ) * ( ( 1 - tx ) * at( i, j ) + tx * at( i + 1, j ) )
	       + ty * ( ( 1 - tx ) * at( i, j + 1 ) + tx * at( i + 1, j + 1 ) );
}

float paramFor( double value, double low, double high )
{
	return static_cast< float >( ( value - low ) / ( high - low ) );
}

//===========================================================================
// --flat
//===========================================================================
int runFlat( const Perturb& perturb )
{
	std::printf( "\n=== flat: at Crumple 0 the sheet is flat and the output is the picture\n" );
	for( const auto& size : { std::pair< int, int > { 480, 270 }, std::pair< int, int > { 1280, 720 } } )
	{
		const Floats card = buildCard( size.first, size.second );
		Rig rig;
		if( !rig.Init( size.first, size.second, &card ) )
			return 1;
		rig.Set( PT_CRUMPLE, 0.0f );
		rig.Render( 3 );
		const Floats out = rig.Output();
		double worst = 0.0;
		for( size_t i = 0; i < out.size(); ++i )
			worst = std::max( worst, std::fabs( out[ i ] - ( card[ i ] + ( i % 4 == 3 ? 0.0 : perturb.flatSlack ) ) ) );
		//Every term is normalised to the flat sheet, so it is the picture to
		//within a GPU division -- a few units in the last place.
		Check( worst < 1e-5, fmt( "%dx%d, every other control at its default: within %.1e of the picture (bound 1e-5)",
		                          size.first, size.second, worst ) );
	}
	return Verdict();
}

//===========================================================================
// --isometry
//===========================================================================
int runIsometry( const Perturb& perturb )
{
	std::printf( "\n=== isometry: across one straight crease the print pulls in by 1/2 s^2 of its width\n" );

	//A straight ridge is developable: the sheet can bend along it without
	//stretching anywhere, so the least-squares stretch is exact there. Across
	//it, d u_x / dx = -1/2 h_x^2 + a constant, and over an interval spanning
	//the whole ridge the print's ends come together by 1/2 s^2 2W MORE than
	//over an equal interval of flat sheet -- the constant, which is the
	//pinned edges' share, cancels.
	const double x0 = 0.5, W = 0.12, s = 0.5;
	const double pulled = perturb.strainFactor * 0.5 * s * s * 2.0 * W;

	for( int detail = 0; detail < kDetailCount; ++detail )
	{
		Rig rig;
		if( !rig.Init( 960, 540, nullptr ) )
			return 1;
		rig.Bare();
		rig.Set( PT_DETAIL, static_cast< float >( detail ) );
		rig.plugin.SetSheetForTest( RidgeSheet( static_cast< float >( x0 ), static_cast< float >( W ), static_cast< float >( s ), static_cast< float >( rig.width ) / rig.height ) );
		rig.Render( 1 );

		const Floats field = rig.StretchField();
		const int nx = rig.plugin.GridWidth(), ny = rig.plugin.GridHeight();
		const double aspect = 960.0 / 540.0;

		double worst = 0.0, sideways = 0.0;
		for( double y : { 0.2, 0.5, 0.8 } )
		{
			const double D     = 0.1;
			const double a     = x0 - W - D, b = x0 + W + D;
			const double creased = stretchAt( field, nx, ny, aspect, b, y, 0 ) - stretchAt( field, nx, ny, aspect, a, y, 0 );
			const double c     = 1.0, d = c + ( b - a );
			const double plain = stretchAt( field, nx, ny, aspect, d, y, 0 ) - stretchAt( field, nx, ny, aspect, c, y, 0 );
			worst              = std::max( worst, std::fabs( ( plain - creased ) - pulled ) );
			for( double x = 0.05; x < aspect; x += 0.02 )
				sideways = std::max( sideways, std::fabs( stretchAt( field, nx, ny, aspect, x, y, 1 ) ) );
		}

		//The strain is drawn at four samples a grid cell and averaged, so a
		//facet's edge lands to within half a coarse pixel -- the ridge's
		//width, and so its pull, is right to one coarse pixel's worth of
		//1/2 s^2. On top of that, float and the ringing a spectral solve puts
		//next to the facet edges (measured 0.3% of the pull at the finer
		//Details). The bound is the larger of 1% and that pixel's worth.
		const double coarsePixel = aspect / std::min( 960, 2 * nx );
		const double bound       = std::max( 0.01 * pulled, 1.1 * 0.5 * s * s * coarsePixel );
		Check( worst < bound,
		       fmt( "Detail %d (%dx%d): pulled in %.5f more than flat sheet, expected %.5f, worst error %.1e (bound %.1e)",
		            kDetailCells[ detail ], nx, ny, pulled, pulled, worst, bound ) );
		Check( sideways < 1e-4 * pulled / W, fmt( "Detail %d: no sideways stretch along a straight crease (%.1e)",
		                                          kDetailCells[ detail ], sideways ) );
	}

	//And the composite reads the print from x - u: a coordinate card, read
	//back, IS the material point each pixel shows.
	{
		Rig rig;
		const Floats card = coordinateCard( 960, 540 );
		if( !rig.Init( 960, 540, &card ) )
			return 1;
		rig.Bare();
		rig.Set( PT_AMBIENT, 1.0f );//light everything the same, so the output is the card
		rig.Set( PT_STRETCH, 0.5f );//physical
		rig.plugin.SetSheetForTest( RidgeSheet( static_cast< float >( x0 ), static_cast< float >( W ), static_cast< float >( s ), static_cast< float >( rig.width ) / rig.height ) );
		rig.Render( 1 );
		const Floats out   = rig.Output();
		const Floats field = rig.StretchField();
		const int nx = rig.plugin.GridWidth(), ny = rig.plugin.GridHeight();
		double worst = 0.0, most = 0.0;
		for( int x = 40; x < 920; x += 7 )
		{
			const int y        = 270;
			const double px    = ( x + 0.5 ) / 540.0;
			const double u     = stretchAt( field, nx, ny, 960.0 / 540.0, px, ( y + 0.5 ) / 540.0, 0 );
			const double shown = out[ ( static_cast< size_t >( y ) * 960 + x ) * 4 ];
			worst              = std::max( worst, std::fabs( shown - ( px - u ) ) );
			most               = std::max( most, std::fabs( u ) );
		}
		//A texel of the coordinate card is 1/540; the sample lands between
		//texels and the card is linear, so it is exact to float.
		Check( worst < 1e-5 && most > 1e-3,
		       fmt( "the print is read from x - u: within %.1e across a row (u up to %.4f)", worst, most ) );
	}
	return Verdict();
}

//===========================================================================
// --lambert
//===========================================================================
int runLambert( const Perturb& perturb )
{
	std::printf( "\n=== lambert: each facet of a known crease is lit by the formula\n" );
	const double x0 = 0.8, W = 0.25, s = 0.4, ambient = 0.35;
	const double elevation = 35.0, azimuth = 30.0;
	for( const auto& size : { std::pair< int, int > { 480, 270 }, std::pair< int, int > { 1280, 720 } } )
	{
		const Floats white = flatCard( size.first, size.second, 1.0f );
		Rig rig;
		if( !rig.Init( size.first, size.second, &white ) )
			return 1;
		rig.Bare();
		rig.Set( PT_AMBIENT, static_cast< float >( ambient ) );
		rig.Set( PT_LAMP_ELEVATION, paramFor( elevation, 5.0, 90.0 ) );
		rig.Set( PT_LAMP_AZIMUTH, paramFor( azimuth, 0.0, 360.0 ) );
		rig.plugin.SetSheetForTest( RidgeSheet( static_cast< float >( x0 ), static_cast< float >( W ), static_cast< float >( s ), static_cast< float >( rig.width ) / rig.height ) );
		rig.Render( 1 );
		const Floats out = rig.Output();

		const double e = ( elevation + perturb.lampTilt ) * kPi / 180.0, a = azimuth * kPi / 180.0;
		const double L[ 3 ] = { std::cos( e ) * std::cos( a ), std::cos( e ) * std::sin( a ), std::sin( e ) };
		double worst = 0.0, lo = 10.0, hi = 0.0;
		for( int side = -1; side <= 1; side += 2 )
		{
			//The tent rises towards the spine: h = s ( W - |x - x0| ).
			const double hx   = -side * s;
			const double norm = std::sqrt( hx * hx + 1.0 );
			const double nL   = ( -hx * L[ 0 ] + L[ 2 ] ) / norm;
			const double want = ( ambient + ( 1.0 - ambient ) * std::max( nL, 0.0 ) ) / ( ambient + ( 1.0 - ambient ) * L[ 2 ] );
			const double x    = x0 + side * 0.5 * W;
			const int px      = static_cast< int >( x * size.second );
			for( int y = size.second / 4; y < size.second * 3 / 4; y += 5 )
			{
				const double got = out[ ( static_cast< size_t >( y ) * size.first + px ) * 4 ];
				worst            = std::max( worst, std::fabs( got - want ) );
				lo               = std::min( lo, got );
				hi               = std::max( hi, got );
			}
		}
		Check( worst < 1e-5, fmt( "%dx%d: the two facets at %.4f and %.4f, worst error %.1e (bound 1e-5)", size.first,
		                          size.second, lo, hi, worst ) );
	}
	return Verdict();
}

//===========================================================================
// --shadow
//===========================================================================
int runShadow( const Perturb& perturb )
{
	std::printf( "\n=== shadow: a ridge of height H under a lamp at elevation e throws a shadow H / tan e long\n" );
	const double x0 = 1.0, W = 0.1, s = 0.8, ambient = 0.35, elevation = 20.0;
	const double H = s * W, tanE = std::tan( elevation * kPi / 180.0 );
	const double edge = x0 - perturb.shadowFactor * H / tanE;
	for( const auto& size : { std::pair< int, int > { 640, 360 }, std::pair< int, int > { 1920, 1080 } } )
	{
		const Floats white = flatCard( size.first, size.second, 1.0f );
		Rig rig;
		if( !rig.Init( size.first, size.second, &white ) )
			return 1;
		rig.Bare();
		rig.Set( PT_SHADOWS, 1.0f );
		rig.Set( PT_AMBIENT, static_cast< float >( ambient ) );
		rig.Set( PT_LAMP_ELEVATION, paramFor( elevation, 5.0, 90.0 ) );
		rig.Set( PT_LAMP_AZIMUTH, 0.0f );//the lamp off to the right: shadows fall left
		rig.plugin.SetSheetForTest( RidgeSheet( static_cast< float >( x0 ), static_cast< float >( W ), static_cast< float >( s ), static_cast< float >( rig.width ) / rig.height ) );
		rig.Render( 1 );
		const Floats out = rig.Output();

		const double dark = ambient / ( ambient + ( 1.0 - ambient ) * std::sin( elevation * kPi / 180.0 ) );
		const int y       = size.second / 2;
		//Walk left from the foot of the ridge along the flat sheet to where it
		//is lit again: the first pixel back above halfway from shadow to lit.
		double found = -1.0;
		for( int x = static_cast< int >( ( x0 - W ) * size.second ) - 2; x > 0; --x )
		{
			const double v = out[ ( static_cast< size_t >( y ) * size.first + x ) * 4 ];
			if( v > 0.5 * ( dark + 1.0 ) )
			{
				found = ( x + 1.0 ) / size.second;
				break;
			}
		}
		//The march steps the ray along ShadowReach / steps; a peak can fall
		//between two steps and read up to s * step / 2 low, which shortens
		//the shadow by that over tan e. Plus a pixel for where the edge is.
		const double reach = std::min( 2.0 * H / tanE + 0.002, 0.6 );
		const double step  = reach / ShadowSteps( static_cast< float >( reach ), size.second );
		const double bound = s * step / 2.0 / tanE + 1.5 / size.second;
		Check( found > 0.0 && std::fabs( found - edge ) < bound,
		       fmt( "%dx%d: shadow ends at x = %.4f, H / tan e puts it at %.4f (bound %.4f)", size.first, size.second,
		            found, edge, bound ) );
	}
	return Verdict();
}

//===========================================================================
// --monotone
//===========================================================================
int runMonotone( const Perturb& perturb )
{
	std::printf( "\n=== monotone: raising Crumple only ever raises the sheet; a sheet is the same sheet at any Crumple\n" );
	SheetSettings settings;
	settings.seed   = 7;
	settings.layers = 4;
	settings.scale  = 0.3f;
	SheetSettings other = settings;
	other.seed          = perturb.reseeded ? 8 : 7;

	const std::vector< Layer > a = BuildLayers( settings );
	const std::vector< Layer > b = BuildLayers( other );

	bool same = a.size() == b.size();
	size_t junctions = 0, facets = 0;
	for( size_t l = 0; same && l < a.size(); ++l )
	{
		same = a[ l ].junctions.size() == b[ l ].junctions.size() && a[ l ].corners == b[ l ].corners;
		for( size_t i = 0; same && i < a[ l ].junctions.size(); ++i )
			same = a[ l ].junctions[ i ].x == b[ l ].junctions[ i ].x && a[ l ].junctions[ i ].height == b[ l ].junctions[ i ].height;
		junctions += a[ l ].junctions.size();
		facets += a[ l ].corners.size() / 3;
	}
	Check( same, fmt( "the same seed builds the same %zu junctions and %zu facets, bit for bit", junctions, facets ) );

	//Every facet's corners rise monotonically with Crumple, from flat at 0
	//to whole at 1. Measured on the facets the plugin would draw.
	int backwards = 0;
	double flatAt0 = 0.0, wholeAt1 = 0.0;
	std::vector< Facet > last;
	for( int step = 0; step <= 100; ++step )
	{
		SheetSettings at = other;
		at.crumple       = step / 100.0f;
		const std::vector< Facet > now = FacetsFrom( b, at );
		if( !last.empty() )
			for( size_t f = 0; f < now.size(); ++f )
				for( int k = 0; k < 3; ++k )
					if( std::fabs( now[ f ].z[ k ] ) < std::fabs( last[ f ].z[ k ] ) - 1e-9f )
						++backwards;
		if( step == 0 )
			for( const Facet& f : now )
				for( float z : f.z )
					flatAt0 = std::max( flatAt0, static_cast< double >( std::fabs( z ) ) );
		last = now;
	}
	for( const Layer& layer : b )
		for( const Junction& j : layer.junctions )
			wholeAt1 = std::max( wholeAt1, 1.0 - Formed( j, 1.0f ) );
	Check( backwards == 0, fmt( "no corner of any facet ever sinks as Crumple rises (%d did)", backwards ) );
	Check( flatAt0 == 0.0 && wholeAt1 == 0.0, fmt( "flat at Crumple 0 (%.1e), every junction whole at 1 (%.1e short)", flatAt0, wholeAt1 ) );

	//The facets are a triangulation: every facet counter-clockwise, and they
	//cover the frame -- the sum of the areas of the facets over the frame
	//region is the frame's area, checked by sampling.
	int clockwise = 0;
	for( size_t l = 0; l < b.size(); ++l )
		for( size_t t = 0; t < b[ l ].corners.size(); t += 3 )
		{
			const Junction& p = b[ l ].junctions[ b[ l ].corners[ t ] ];
			const Junction& q = b[ l ].junctions[ b[ l ].corners[ t + 1 ] ];
			const Junction& r = b[ l ].junctions[ b[ l ].corners[ t + 2 ] ];
			clockwise += ( q.x - p.x ) * ( r.y - p.y ) - ( q.y - p.y ) * ( r.x - p.x ) <= 0.0f;
		}
	int uncovered = 0, doubled = 0;
	for( const Layer& layer : b )
		for( int sy = 0; sy < 60; ++sy )
			for( int sx = 0; sx < 100; ++sx )
			{
				const float x = ( sx + 0.5f ) / 100.0f * other.aspect, y = ( sy + 0.5f ) / 60.0f;
				int inside = 0;
				for( size_t t = 0; t < layer.corners.size(); t += 3 )
				{
					const Junction* v[ 3 ] = { &layer.junctions[ layer.corners[ t ] ], &layer.junctions[ layer.corners[ t + 1 ] ],
						                       &layer.junctions[ layer.corners[ t + 2 ] ] };
					bool in = true;
					for( int k = 0; k < 3 && in; ++k )
					{
						const Junction& a0 = *v[ k ];
						const Junction& a1 = *v[ ( k + 1 ) % 3 ];
						in = ( a1.x - a0.x ) * ( y - a0.y ) - ( a1.y - a0.y ) * ( x - a0.x ) >= 0.0f;
					}
					inside += in;
				}
				uncovered += inside == 0;
				doubled += inside > 1;
			}
	Check( clockwise == 0 && uncovered == 0,
	       fmt( "every facet counter-clockwise (%d not); every one of 6000 frame points per layer on a facet (%d missed, %d on an "
	            "edge)",
	            clockwise, uncovered, doubled ) );
	return Verdict();
}

//===========================================================================
// --negative
//===========================================================================
int runNegative()
{
	struct Case
	{
		const char* name;
		int ( *check )( const Perturb& );
		Perturb perturb;
		const char* what;
	};
	std::vector< Case > cases;
	{
		Perturb p;
		p.flatSlack = 1.0 / 255.0;
		cases.push_back( { "flat", runFlat, p, "expect one 8-bit code value more" } );
	}
	{
		Perturb p;
		p.strainFactor = 1.05;
		cases.push_back( { "isometry", runIsometry, p, "expect the print pulled in 5% further" } );
	}
	{
		Perturb p;
		p.lampTilt = 1.0;
		cases.push_back( { "lambert", runLambert, p, "expect the lamp one degree higher" } );
	}
	{
		Perturb p;
		p.shadowFactor = 1.15;
		cases.push_back( { "shadow", runShadow, p, "expect shadows 15% longer" } );
	}
	{
		Perturb p;
		p.reseeded = true;
		cases.push_back( { "monotone", runMonotone, p, "compare against the next seed's sheet" } );
	}

	int unfalsifiable = 0;
	for( const Case& c : cases )
	{
		std::printf( "\n=== negative control: %s -- %s\n", c.name, c.what );
		const int before = g_failures;
		g_failures       = 0;
		c.check( c.perturb );
		const int observed = g_failures;
		g_failures         = before;
		if( observed > 0 )
			std::printf( "  ok    %s failed %d check%s, as it must\n", c.name, observed, observed == 1 ? "" : "s" );
		else
		{
			std::printf( "  FAIL  %s PASSED against a wrong model -- it cannot fail, so it is not a check\n", c.name );
			++unfalsifiable;
		}
	}
	std::printf( "\nnegative controls: %zu perturbations, %d of them undetected\n", cases.size(), unfalsifiable );
	std::printf( "\n  %s\n", unfalsifiable == 0 ? "PASS" : "FAIL" );
	return unfalsifiable == 0 ? 0 : 1;
}

//===========================================================================
// --bench
//===========================================================================
int runBench()
{
	struct Size
	{
		int w, h;
		const char* name;
		int detail;
	};
	const Size sizes[] = { { 1280, 720, "720p", 1 }, { 1920, 1080, "1080p", 1 }, { 3840, 2160, "4K", 1 },
		                   { 1920, 1080, "1080p", 2 } };
	for( const Size& size : sizes )
		for( int moving = 0; moving < 2; ++moving )
		{
			Rig rig;
			if( !rig.Init( size.w, size.h ) )
				return 1;
			rig.Set( PT_DETAIL, static_cast< float >( size.detail ) );
			rig.Set( PT_LAYERS, 3.0f );//four generations: the busy case
			if( !rig.Render( 10 ) )
				return 1;
			glFinish();
			constexpr int kTimed = 40;
			const auto start     = std::chrono::steady_clock::now();
			for( int i = 0; i < kTimed; ++i )
			{
				//Moving: Crumple changes every frame, so the sheet, the strain
				//and the stretch are all rebuilt -- an automated crumple. Still:
				//the sheet is built once and each frame is the composite.
				if( moving )
					rig.Set( PT_CRUMPLE, 0.5f + 0.4f * static_cast< float >( i ) / kTimed );
				if( !rig.Render( 1 ) )
					return 1;
			}
			glFinish();
			const double ms =
				std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() / kTimed;
			std::printf( "  %-6s Detail %-4d 4 layers, %-6s %6.2f ms/frame  (%4.1f%% of 60 fps; grid %dx%d)\n", size.name,
			             kDetailCells[ size.detail ], moving ? "moving" : "still", ms, 100.0 * ms / ( 1000.0 / 60.0 ),
			             rig.plugin.GridWidth(), rig.plugin.GridHeight() );
		}
	return 0;
}

//---------------------------------------------------------------------------
// Parameters by display name, so the automation reads as English.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	float value;
	std::string kind;
};

const char* kindName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_RED: return "red";
	case FF_TYPE_GREEN: return "green";
	case FF_TYPE_BLUE: return "blue";
	case FF_TYPE_XPOS: return "xpos";
	case FF_TYPE_YPOS: return "ypos";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_TEXT: return "text";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( CrumplePlugin& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ),
		                                 kindName( plugin.GetParamType( i ) ) } );
	}
	return list;
}

bool applySetting( CrumplePlugin& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}

	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );

	for( const NamedParameter& parameter : listParameters( plugin ) )
	{
		if( parameter.name != name )
			continue;
		plugin.SetFloatParameter( parameter.index, std::strtof( value.c_str(), nullptr ) );
		return true;
	}

	error = "no parameter called '" + name + "'";
	return false;
}


//---------------------------------------------------------------------------
// --script: one 'frame Parameter Name value' per line. Same format as the
// rest of the fleet, so one filming script drives any of them.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;

		//The name is everything up to the last token: parameters have spaces
		//in them ("Pebble Size") and the value never does.
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 0; i + 1 < track.size(); ++i )
	{
		const auto& a = track[ i ];
		const auto& b = track[ i + 1 ];
		if( frame >= a.first && frame <= b.first )
		{
			if( b.first == a.first )
				return b.second;
			const float t = static_cast< float >( frame - a.first ) / static_cast< float >( b.first - a.first );
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

/// --pipe and --film. Raw RGBA, top row first, one frame at a time, on the
/// synthetic 60 fps clock -- so a stall in ffmpeg cannot show up as the water
/// speeding up afterwards.
int runPipe( int width, int height, const std::string& scriptPath, int filmFrames, bool beat,
             const std::vector< std::string >& settings )
{
	Rig rig;
	if( !rig.Init( width, height ) )
		return 1;
	if( beat )
		rig.feed = AudioFeed::Pulses;

	for( const std::string& setting : settings )
	{
		std::string error;
		if( !applySetting( rig.plugin, setting, error ) )
		{
			std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
			return 2;
		}
	}

	//Resolve the script's names once, and refuse a name that is not a
	//parameter: a misspelling that silently did nothing would film a take
	//that looks deliberate and is wrong.
	std::map< unsigned int, Track > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
		if( !error.empty() )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
		const std::vector< NamedParameter > known = listParameters( rig.plugin );
		for( const auto& entry : tracks )
		{
			bool found = false;
			for( const NamedParameter& parameter : known )
				if( parameter.name == entry.first )
				{
					automation[ parameter.index ] = entry.second;
					found                         = true;
				}
			if( !found )
			{
				std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
				return 2;
			}
		}
	}

	const Floats card = buildCard( width, height );
	std::vector< unsigned char > in( static_cast< size_t >( width ) * height * 4 );
	Floats picture( in.size() );

	for( int index = 0; filmFrames < 0 || index < filmFrames; ++index )
	{
		if( filmFrames < 0 )
		{
			size_t filled = 0;
			while( filled < in.size() )
			{
				const ssize_t got = read( STDIN_FILENO, in.data() + filled, in.size() - filled );
				if( got <= 0 )
					break;
				filled += static_cast< size_t >( got );
			}
			if( filled < in.size() )
				break;

			//Top row first on the wire; bottom row first in GL.
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width * 4; ++x )
					picture[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] =
						in[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
			rig.Upload( picture );
		}
		else if( index == 0 )
			rig.Upload( card );

		for( const auto& track : automation )
			rig.plugin.SetFloatParameter( track.first, valueAt( track.second, index ) );

		if( !rig.Render( 1 ) )
			return 1;

		const Floats out = rig.Output();
		std::vector< unsigned char > bytes( in.size() );
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width * 4; ++x )
				bytes[ static_cast< size_t >( y ) * width * 4 + x ] = static_cast< unsigned char >( std::lround(
					std::clamp( out[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ], 0.0f, 1.0f ) * 255.0f ) );

		size_t written = 0;
		while( written < bytes.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, bytes.data() + written, bytes.size() - written );
			if( put <= 0 )
				return 1;
			written += static_cast< size_t >( put );
		}
	}
	return 0;
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath = "/tmp/crumple.png";
	std::string cardPath;
	std::vector< std::string > settings;
	int width  = 1280;
	int height = 720;
	int frames = 3;
	bool beat  = false;
	std::string mode;
	std::string scriptPath;
	int filmFrames = -1;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			std::printf( "crtest -- render Crumple offline and measure its sheet\n\n"
			             "  --out PATH        render the card and write it here\n"
			             "  --card PATH       write the card itself\n"
			             "  --size WxH        render size (default 1280x720)\n"
			             "  --frames N        frames before reading back (default 3)\n"
			             "  --beat            feed a beat every half second into the Audio buffer\n"
			             "  --set \"Name=V\"    set a parameter by its display name. Repeatable.\n"
			             "  --list            print every parameter and its default, then exit\n"
			             "  --pipe            raw RGBA frames on stdin, raw RGBA frames on stdout\n"
			             "  --film N          N frames of the card, raw RGBA frames on stdout\n"
			             "  --script PATH     parameter cues for --pipe/--film: 'frame Name value'\n\n"
			             "  --flat --isometry --lambert --shadow --monotone --negative --bench\n" );
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--card" && hasNext )
			cardPath = argv[ ++i ];
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--beat" )
			beat = true;
		else if( argument == "--list" )
			mode = "list";
		else if( argument == "--pipe" )
			mode = "pipe";
		else if( argument == "--film" && hasNext )
		{
			mode       = "pipe";
			filmFrames = std::max( 1, std::atoi( argv[ ++i ] ) );
		}
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--flat" || argument == "--isometry" || argument == "--lambert" || argument == "--shadow"
		         || argument == "--monotone" || argument == "--negative" || argument == "--bench" )
			mode = argument.substr( 2 );
		else if( argument == "--size" && hasNext )
		{
			const std::string value = argv[ ++i ];
			const size_t cross      = value.find( 'x' );
			if( cross != std::string::npos )
			{
				width  = std::atoi( value.substr( 0, cross ).c_str() );
				height = std::atoi( value.substr( cross + 1 ).c_str() );
			}
		}
		else
		{
			std::fprintf( stderr, "unknown argument '%s' (try --help)\n", argument.c_str() );
			return 2;
		}
	}

	if( mode == "list" )
	{
		CrumplePlugin plugin;
		std::printf( "%-3s %-18s %-9s %s\n", "id", "name", "kind", "default" );
		for( const NamedParameter& parameter : listParameters( plugin ) )
			std::printf( "%-3u %-18s %-9s %.4f\n", parameter.index, parameter.name.c_str(), parameter.kind.c_str(),
			             parameter.value );
		return 0;
	}
	if( mode == "monotone" )
		return runMonotone( Perturb {} );

	if( !cardPath.empty() )
	{
		if( !writePng( cardPath, width, height, buildCard( width, height ) ) )
			return 1;
		std::printf( "wrote %s -- the card, %dx%d\n", cardPath.c_str(), width, height );
		return 0;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	int result = 0;
	const Perturb none;
	if( mode == "flat" )
		result = runFlat( none );
	else if( mode == "isometry" )
		result = runIsometry( none );
	else if( mode == "lambert" )
		result = runLambert( none );
	else if( mode == "shadow" )
		result = runShadow( none );
	else if( mode == "negative" )
		result = runNegative();
	else if( mode == "bench" )
		result = runBench();
	else if( mode == "pipe" )
		result = runPipe( width, height, scriptPath, filmFrames, beat, settings );
	else
	{
		Rig rig;
		if( !rig.Init( width, height ) )
			result = 1;
		else
		{
			for( const std::string& setting : settings )
			{
				std::string error;
				if( !applySetting( rig.plugin, setting, error ) )
				{
					std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
					return 2;
				}
			}
			if( beat )
				rig.feed = AudioFeed::Pulses;
			if( !rig.Render( std::max( frames, 1 ) ) )
				result = 1;
			else if( writePng( outPath, width, height, rig.Output() ) )
				std::printf( "wrote %s -- %dx%d, %d frames\n", outPath.c_str(), width, height, frames );
			else
				result = 1;
		}
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
