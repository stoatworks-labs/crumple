#include "Crumple.h"

#include "Diag.h"
#include "GLState.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <string>

using namespace ffglex;

namespace crumple
{
namespace
{
constexpr float kPi = 3.14159265358979324f;

std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

const char* const kDetailNames[] = { "256", "512", "1024" };
const char* const kLayerNames[]  = { "1", "2", "3", "4" };
const char* const kViewNames[]   = { "Picture", "Height", "Normals", "Stretch" };

/// One vertex of the sheet: corner, height, then the facet's slope, its
/// three edges' creases and its three corners' altitudes, repeated on each
/// of its three vertices so the shader can hand them on flat.
struct SheetVertex
{
	float x, y, z;
	float hx, hy;
	float crease[ 3 ];
	float altitude[ 3 ];
};

constexpr int kClockVotes       = 4;
constexpr double kMaxFrameDelta = 0.25;

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

int optionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

void setInts( const FFGLShader& shader, const char* name, int a, int b )
{
	glUniform2i( glGetUniformLocation( shader.GetGLID(), name ), a, b );
}

int log2Exact( int value )
{
	int bits = 0;
	while( ( 1 << bits ) < value )
		++bits;
	return bits;
}

} // namespace

int ShadowSteps( float reach, int pictureHeight )
{
	//A step every pixel and a half -- of a 1080-line picture at most, since
	//past that a shadow's edge is placed more finely than anyone can see and
	//the march costs a sample per step per pixel.
	const float lines = static_cast< float >( std::min( pictureHeight, 1080 ) );
	return std::clamp( static_cast< int >( std::ceil( reach * lines / 1.5f ) ), 16, 96 );
}

namespace
{
/// The solve's grid: `longCells` along the mirrored domain's longer side, and
/// whichever power of two makes the cells closest to square along the other.
void chooseGrid( float aspect, int longCells, int& nx, int& ny )
{
	const bool wide    = aspect >= 1.0f;
	const double ratio = wide ? 1.0 / aspect : aspect;
	const int shortCells =
		std::max( 8, static_cast< int >( std::lround( std::pow( 2.0, std::round( std::log2( longCells * ratio ) ) ) ) ) );
	nx = wide ? longCells : shortCells;
	ny = wide ? shortCells : longCells;
}
} // namespace

static_assert( PT_COUNT - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
               "the About run no longer matches StoatworksAbout.h -- "
               "add or remove a PT_ABOUT_BUTTON_n to match" );

//---------------------------------------------------------------------------
CrumplePlugin::CrumplePlugin()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );
	//Time only drives the audio envelope; the sheet itself is a pure function
	//of the parameters, so a paused host shows a still sheet.
	SetTimeSupported( true );

	//-------------------------------------------------------------------
	// Defaults: a sheet crumpled well and smoothed out part of the way, under
	// a desk lamp from the top left. It reads as crumpled paper the moment
	// it is dropped on a layer.
	//-------------------------------------------------------------------
	params[ PT_CRUMPLE ] = 0.85f;
	params[ PT_FLATTEN ] = 0.4f;
	params[ PT_SCALE ]   = 0.55f;//the first generation's facets 0.24 of the frame
	params[ PT_LAYERS ]  = 2.0f; //three generations
	params[ PT_RELIEF ]  = 0.35f;//0.7 of the sheet as generated
	params[ PT_SEED ]    = 0.0f;
	params[ PT_STRETCH ] = 0.5f; //physical
	params[ PT_DETAIL ]  = 1.0f; //512

	params[ PT_LAMP_ELEVATION ] = 0.41f; //40 degrees
	params[ PT_LAMP_AZIMUTH ]   = 0.375f;//135: the top left
	params[ PT_AMBIENT ]        = 0.4f;
	params[ PT_SHADOWS ]        = 0.8f;
	params[ PT_SHEEN ]          = 0.25f;
	params[ PT_WEAR ]           = 0.4f;
	params[ PT_PAPER_R ]        = 0.97f;
	params[ PT_PAPER_G ]        = 0.96f;
	params[ PT_PAPER_B ]        = 0.93f;

	params[ PT_AUDIO_SCRUNCH ] = 0.0f;
	params[ PT_VIEW ]          = static_cast< float >( View::Picture );
	params[ PT_MIX ]           = 1.0f;

	SetParamInfof( PT_CRUMPLE, "Crumple", FF_TYPE_STANDARD );
	SetParamInfof( PT_FLATTEN, "Flatten", FF_TYPE_STANDARD );
	SetParamInfof( PT_SCALE, "Scale", FF_TYPE_STANDARD );
	SetOptionParamInfo( PT_LAYERS, "Layers", kMaxLayers, params[ PT_LAYERS ] );
	for( int i = 0; i < kMaxLayers; ++i )
		SetParamElementInfo( PT_LAYERS, i, kLayerNames[ i ], static_cast< float >( i ) );
	SetParamInfof( PT_RELIEF, "Relief", FF_TYPE_STANDARD );
	SetParamInfof( PT_SEED, "Seed", FF_TYPE_STANDARD );
	SetParamInfof( PT_STRETCH, "Stretch", FF_TYPE_STANDARD );
	SetOptionParamInfo( PT_DETAIL, "Detail", kDetailCount, params[ PT_DETAIL ] );
	for( int i = 0; i < kDetailCount; ++i )
		SetParamElementInfo( PT_DETAIL, i, kDetailNames[ i ], static_cast< float >( i ) );

	SetParamInfof( PT_LAMP_ELEVATION, "Lamp Elevation", FF_TYPE_STANDARD );
	SetParamInfof( PT_LAMP_AZIMUTH, "Lamp Azimuth", FF_TYPE_STANDARD );
	SetParamInfof( PT_AMBIENT, "Ambient", FF_TYPE_STANDARD );
	SetParamInfof( PT_SHADOWS, "Shadows", FF_TYPE_STANDARD );
	SetParamInfof( PT_SHEEN, "Sheen", FF_TYPE_STANDARD );
	SetParamInfof( PT_WEAR, "Wear", FF_TYPE_STANDARD );
	SetParamInfof( PT_PAPER_R, "Paper", FF_TYPE_RED );
	SetParamInfof( PT_PAPER_G, "Paper_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_PAPER_B, "Paper_Blue", FF_TYPE_BLUE );

	SetBufferParamInfo( PT_AUDIO, "Audio", audio::kBins, FF_USAGE_FFT );
	for( int i = 0; i < audio::kBins; ++i )
		SetParamElementInfo( PT_AUDIO, i, "", 0.0f );
	SetParamInfof( PT_AUDIO_SCRUNCH, "Audio Scrunch", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_VIEW, "View", static_cast< int >( View::Count ), params[ PT_VIEW ] );
	for( int i = 0; i < static_cast< int >( View::Count ); ++i )
		SetParamElementInfo( PT_VIEW, i, kViewNames[ i ], static_cast< float >( i ) );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	for( unsigned int id = PT_CRUMPLE; id <= PT_DETAIL; ++id )
		SetParamGroup( id, "Sheet" );
	for( unsigned int id = PT_LAMP_ELEVATION; id <= PT_PAPER_B; ++id )
		SetParamGroup( id, "Lamp" );
	for( unsigned int id = PT_AUDIO; id <= PT_AUDIO_SCRUNCH; ++id )
		SetParamGroup( id, "Audio" );
	for( unsigned int id = PT_VIEW; id <= PT_MIX; ++id )
		SetParamGroup( id, "Output" );

	SetParamInfo( PT_ABOUT_TEXT, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_TEXT + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( unsigned int id = PT_ABOUT_TEXT; id < PT_COUNT; ++id )
		SetParamGroup( id, "About" );
}

//---------------------------------------------------------------------------
FFResult CrumplePlugin::InitGL( const FFGLViewportStruct* vp )
{
	diag::init();
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer="
	            + glStringOrUnknown( GL_RENDERER ) + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct Stage
	{
		FFGLShader* shader;
		const char* vertex;
		const char* fragment;
		const char* name;
	};
	const Stage stages[] = {
		{ &sheetShader, kSheetVertexShader, kSheetShader, "sheet" },
		{ &strainShader, kVertexShader, kStrainShader, "strain" },
		{ &sourceShader, kVertexShader, kSourceShader, "source" },
		{ &fftShader, kVertexShader, kFFTShader, "fft" },
		{ &solveShader, kVertexShader, kSolveShader, "solve" },
		{ &compositeShader, kVertexShader, kCompositeShader, "composite" },
	};
	for( const Stage& stage : stages )
	{
		if( stage.shader->Compile( stage.vertex, stage.fragment ) )
			continue;
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the plugin will do nothing" );
		FFGLLog::LogToHost( "Crumple: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	glGenVertexArrays( 1, &sheetVAO );
	glGenBuffers( 1, &sheetVBO );
	if( sheetVAO == 0 || sheetVBO == 0 )
	{
		diag::error( "could not create the sheet's vertex array" );
		DeInitGL();
		return FF_FAIL;
	}
	glBindVertexArray( sheetVAO );
	glBindBuffer( GL_ARRAY_BUFFER, sheetVBO );
	const GLsizei stride = sizeof( SheetVertex );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast< const GLvoid* >( offsetof( SheetVertex, x ) ) );
	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast< const GLvoid* >( offsetof( SheetVertex, z ) ) );
	glEnableVertexAttribArray( 2 );
	glVertexAttribPointer( 2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast< const GLvoid* >( offsetof( SheetVertex, hx ) ) );
	glEnableVertexAttribArray( 3 );
	glVertexAttribPointer( 3, 3, GL_FLOAT, GL_FALSE, stride,
	                       reinterpret_cast< const GLvoid* >( offsetof( SheetVertex, crease ) ) );
	glEnableVertexAttribArray( 4 );
	glVertexAttribPointer( 4, 3, GL_FLOAT, GL_FALSE, stride,
	                       reinterpret_cast< const GLvoid* >( offsetof( SheetVertex, altitude ) ) );
	glBindVertexArray( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
GLuint CrumplePlugin::MakeTwiddles( int length )
{
	std::vector< float > table( static_cast< size_t >( length ) * 2 );
	for( int m = 0; m < length; ++m )
	{
		const double angle = -2.0 * 3.14159265358979323846 * static_cast< double >( m ) / length;
		table[ m * 2 + 0 ] = static_cast< float >( std::cos( angle ) );
		table[ m * 2 + 1 ] = static_cast< float >( std::sin( angle ) );
	}
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RG32F, length, 1, 0, GL_RG, GL_FLOAT, table.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

//---------------------------------------------------------------------------
bool CrumplePlugin::ensureBuffers( GLsizei width, GLsizei height, int nx, int ny )
{
	if( nx != gridWidth || ny != gridHeight )
	{
		if( twiddleX != 0 )
			glDeleteTextures( 1, &twiddleX );
		if( twiddleY != 0 )
			glDeleteTextures( 1, &twiddleY );
		twiddleX = MakeTwiddles( nx );
		twiddleY = MakeTwiddles( ny );
		if( twiddleX == 0 || twiddleY == 0 )
			return false;
	}

	//All 32-bit. The height is a sum of up to 160 creases and its gradient is
	//what the lamp and the strain both read; the strain is squared slopes
	//averaged down a mip chain; the transforms sum over the whole grid.
	//The strain only has to be right on AVERAGE over each grid cell -- the
	//solve never sees finer than that -- so it is built from a second, coarse
	//drawing of the sheet at four samples a cell, not from the picture-sized
	//one. At 4K that was most of the cost of a moving sheet.
	const GLsizei coarseWidth  = std::min( width, static_cast< GLsizei >( 2 * nx ) );
	const GLsizei coarseHeight = std::min( height, static_cast< GLsizei >( 2 * ny ) );
	bool ok = sheet.Ensure( width, height, GL_RGBA32F, PassBuffer::Sampling::Linear )
	          && coarse.Ensure( coarseWidth, coarseHeight, GL_RGBA32F, PassBuffer::Sampling::Nearest )
	          && strain.Ensure( coarseWidth, coarseHeight, GL_RGBA32F, PassBuffer::Sampling::Mipmapped );
	for( PassBuffer& buffer : spectrum )
		ok = ok && buffer.Ensure( nx, ny, GL_RGBA32F, PassBuffer::Sampling::Nearest );
	for( PassBuffer& buffer : stretch )
		ok = ok && buffer.Ensure( nx, ny, GL_RGBA32F, PassBuffer::Sampling::Linear, PassBuffer::Wrap::Repeat );
	if( !ok )
		return false;

	gridWidth  = nx;
	gridHeight = ny;
	return true;
}

//---------------------------------------------------------------------------
void CrumplePlugin::DrawSheet( PassBuffer& target, GLsizei vertexCount, float aspect, float markWidth )
{
	ScopedFBOBinding fbo( target.GetGLID(), ScopedFBOBinding::RB_REVERT );
	target.ResizeViewPort();
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	//Additive: each pixel gets one facet's plane from each layer, and the
	//layers sum.
	setAdditiveBlend();
	ScopedShaderBinding shader( sheetShader.GetGLID() );
	sheetShader.Set( "Aspect", aspect );
	sheetShader.Set( "MarkWidth", markWidth );
	glBindVertexArray( sheetVAO );
	glDrawArrays( GL_TRIANGLES, 0, vertexCount );
	glBindVertexArray( 0 );
	glDisable( GL_BLEND );
}

//---------------------------------------------------------------------------
void CrumplePlugin::UpdateClock()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;
	const double raw = hostTime;

	//Rosette's vote on whether the host's clock is in seconds or milliseconds.
	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;
			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}
	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;
	now          = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale : wallNow - wallStart;
}

//---------------------------------------------------------------------------
void CrumplePlugin::Transform( PassBuffer ( &buffers )[ 2 ], int& current, float direction )
{
	ScopedShaderBinding shader( fftShader.GetGLID() );
	fftShader.Set( "Source", 0 );
	fftShader.Set( "Twiddles", 1 );
	fftShader.Set( "Direction", direction );

	struct Axis
	{
		int horizontal, length;
		GLuint twiddles;
	};
	const Axis axes[ 2 ] = { { 1, gridWidth, twiddleX }, { 0, gridHeight, twiddleY } };
	for( const Axis& axis : axes )
	{
		fftShader.Set( "Horizontal", axis.horizontal );
		fftShader.Set( "Length", axis.length );
		const int stages = log2Exact( axis.length );
		for( int s = 1; s <= stages; ++s )
		{
			ScopedFBOBinding fbo( buffers[ 1 - current ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			glViewport( 0, 0, gridWidth, gridHeight );
			//Interleaved, activate then bind, per unit: every Scoped* binding
			//clears on exit, on whichever unit is active then.
			ScopedSamplerActivation sampler0( 0 );
			Scoped2DTextureBinding texture( buffers[ current ].TextureID() );
			ScopedSamplerActivation sampler1( 1 );
			Scoped2DTextureBinding table( axis.twiddles );

			fftShader.Set( "Span", 1 << s );
			quad.Draw();
			current = 1 - current;
		}
	}
}

//---------------------------------------------------------------------------
FFResult CrumplePlugin::ProcessOpenGL( ProcessOpenGLStruct* pgl )
{
	if( pgl == nullptr || pgl->numInputTextures < 1 || pgl->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;
	const FFGLTextureStruct& source = *( pgl->inputTextures[ 0 ] );
	const GLsizei width             = static_cast< GLsizei >( source.Width );
	const GLsizei height            = static_cast< GLsizei >( source.Height );
	if( width <= 0 || height <= 0 )
		return FF_FAIL;

	ScopedGLState restore;
	const GLint* hostViewport = restore.saved.viewport;
	glDisable( GL_BLEND );

	//-------------------------------------------------------------------
	// Audio: the level adds to Crumple, and a kick adds more.
	//-------------------------------------------------------------------
	UpdateClock();
	const double dt = lastNow >= 0.0 ? std::clamp( now - lastNow, 0.0, kMaxFrameDelta ) : 0.0;
	lastNow         = now;
	{
		float bins[ audio::kBins ] = {};
		int binCount              = 0;
		if( const ParamInfo* info = FindParamInfo( PT_AUDIO ) )
		{
			binCount = static_cast< int >( std::min< size_t >( info->elements.size(), audio::kBins ) );
			for( int i = 0; i < binCount; ++i )
				bins[ i ] = info->elements[ static_cast< size_t >( i ) ].value;
		}
		analyser.Update( bins, binCount, static_cast< float >( dt ), audio::Settings {} );
	}

	const float aspect  = static_cast< float >( width ) / static_cast< float >( height );
	const float scrunch = ScrunchFromParam( params[ PT_AUDIO_SCRUNCH ] );
	const float crumple =
		std::clamp( CrumpleFromParam( params[ PT_CRUMPLE ] ) + scrunch * ( 0.5f * analyser.Level() + 0.5f * analyser.Kick() ),
		            0.0f, 1.0f );

	//-------------------------------------------------------------------
	// The sheet: the layers (kept until their shape changes), risen as far as
	// Crumple says, into the vertex buffer.
	//-------------------------------------------------------------------
	SheetSettings settings;
	settings.seed    = SeedFromParam( params[ PT_SEED ] );
	settings.layers  = optionIndex( params[ PT_LAYERS ], kMaxLayers ) + 1;
	settings.scale   = ScaleFromParam( params[ PT_SCALE ] );
	settings.relief  = ReliefFromParam( params[ PT_RELIEF ] );
	settings.crumple = crumple;
	settings.flatten = FlattenFromParam( params[ PT_FLATTEN ] );
	settings.aspect  = aspect;

	int nx = 0, ny = 0;
	chooseGrid( aspect, kDetailCells[ optionIndex( params[ PT_DETAIL ], kDetailCount ) ], nx, ny );

	const bool dirty = !built || forcing || settings.seed != builtFor.seed || settings.layers != builtFor.layers
	                   || settings.scale != builtFor.scale || settings.relief != builtFor.relief
	                   || settings.crumple != builtFor.crumple || settings.flatten != builtFor.flatten
	                   || settings.aspect != builtFor.aspect || width != builtWidth || height != builtHeight
	                   || nx != gridWidth || ny != gridHeight;

	if( dirty )
	{
		if( forcing )
			drawn = forced;
		else
		{
			if( !haveLayers || settings.seed != layersFor.seed || settings.layers != layersFor.layers
			    || settings.scale != layersFor.scale || settings.aspect != layersFor.aspect )
			{
				layers     = BuildLayers( settings );
				layersFor  = settings;
				haveLayers = true;
			}
			drawn = FacetsFrom( layers, settings );
		}

		std::vector< SheetVertex > vertices;
		vertices.reserve( drawn.size() * 3 );
		tallest = 0.0f;
		for( const Facet& f : drawn )
		{
			//The facet's plane, h = z0 + hx ( x - x0 ) + hy ( y - y0 ).
			const double ux = f.x[ 1 ] - f.x[ 0 ], uy = f.y[ 1 ] - f.y[ 0 ], uz = f.z[ 1 ] - f.z[ 0 ];
			const double vx = f.x[ 2 ] - f.x[ 0 ], vy = f.y[ 2 ] - f.y[ 0 ], vz = f.z[ 2 ] - f.z[ 0 ];
			const double det = ux * vy - uy * vx;
			if( std::fabs( det ) < 1e-14 )
				continue;
			const float hx = static_cast< float >( ( uz * vy - uy * vz ) / det );
			const float hy = static_cast< float >( ( ux * vz - uz * vx ) / det );

			//Each corner's distance to the edge opposite it: twice the area over
			//that edge's length.
			float altitude[ 3 ];
			for( int k = 0; k < 3; ++k )
			{
				const int a = ( k + 1 ) % 3, b = ( k + 2 ) % 3;
				const double ex = f.x[ b ] - f.x[ a ], ey = f.y[ b ] - f.y[ a ];
				altitude[ k ]   = static_cast< float >( std::fabs( det ) / std::max( std::sqrt( ex * ex + ey * ey ), 1e-12 ) );
			}

			for( int k = 0; k < 3; ++k )
			{
				vertices.push_back( SheetVertex { f.x[ k ], f.y[ k ], f.z[ k ], hx, hy,
				                                  { f.crease[ 0 ], f.crease[ 1 ], f.crease[ 2 ] },
				                                  { altitude[ 0 ], altitude[ 1 ], altitude[ 2 ] } } );
				tallest = std::max( tallest, std::fabs( f.z[ k ] ) );
			}
		}
		glBindBuffer( GL_ARRAY_BUFFER, sheetVBO );
		glBufferData( GL_ARRAY_BUFFER, static_cast< GLsizeiptr >( vertices.size() * sizeof( SheetVertex ) ), vertices.data(),
		              GL_STREAM_DRAW );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );

		if( !ensureBuffers( width, height, nx, ny ) )
		{
			diag::error( "could not allocate the pass buffers" );
			return FF_FAIL;
		}

		//-------------------------------------------------------------------
		// 1. The sheet.
		//-------------------------------------------------------------------
		//A crease mark about a third of a millimetre on an A4 sheet, never
		//thinner than most of a pixel.
		DrawSheet( sheet, static_cast< GLsizei >( vertices.size() ), aspect,
		           std::max( 0.0012f, 0.7f / static_cast< float >( height ) ) );
		DrawSheet( coarse, static_cast< GLsizei >( vertices.size() ), aspect, 0.0012f );

		//-------------------------------------------------------------------
		// 2. The strain, and its mip chain.
		//-------------------------------------------------------------------
		{
			ScopedFBOBinding fbo( strain.GetGLID(), ScopedFBOBinding::RB_REVERT );
			strain.ResizeViewPort();
			ScopedShaderBinding shader( strainShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			Scoped2DTextureBinding texture( coarse.TextureID() );
			strainShader.Set( "SheetTexture", 0 );
			quad.Draw();
		}
		strain.GenerateMipmaps();

		//-------------------------------------------------------------------
		// 3-6. The stretch.
		//-------------------------------------------------------------------
		const float domainX = 2.0f * aspect, domainY = 2.0f;
		{
			ScopedFBOBinding fbo( spectrum[ 0 ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			glViewport( 0, 0, gridWidth, gridHeight );
			ScopedShaderBinding shader( sourceShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			Scoped2DTextureBinding texture( strain.TextureID() );
			sourceShader.Set( "StrainTexture", 0 );
			setInts( sourceShader, "GridSize", gridWidth, gridHeight );
			sourceShader.Set( "Domain", domainX, domainY );
			sourceShader.Set( "Aspect", aspect );
			const float pixelsPerCell = static_cast< float >( strain.GetHeight() ) * domainY / static_cast< float >( gridHeight );
			sourceShader.Set( "Lod", std::clamp( std::log2( std::max( pixelsPerCell, 1.0f ) ), 0.0f, strain.MaxMipLevel() ) );
			quad.Draw();
		}

		int current = 0;
		Transform( spectrum, current, -1.0f );

		{
			ScopedFBOBinding fbo( stretch[ 0 ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			glViewport( 0, 0, gridWidth, gridHeight );
			ScopedShaderBinding shader( solveShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			Scoped2DTextureBinding texture( spectrum[ current ].TextureID() );
			solveShader.Set( "Spectrum", 0 );
			setInts( solveShader, "GridSize", gridWidth, gridHeight );
			solveShader.Set( "Domain", domainX, domainY );
			solveShader.Set( "Norm", 1.0f / ( static_cast< float >( gridWidth ) * static_cast< float >( gridHeight ) ) );
			quad.Draw();
		}

		int landed = 0;
		Transform( stretch, landed, 1.0f );
		stretchIndex = landed;

		builtFor    = settings;
		builtWidth  = width;
		builtHeight = height;
		built       = true;
	}

	//-------------------------------------------------------------------
	// 7. Composite.
	//-------------------------------------------------------------------
	const float elevation = LampElevationFromParam( params[ PT_LAMP_ELEVATION ] ) * kPi / 180.0f;
	const float azimuth   = LampAzimuthFromParam( params[ PT_LAMP_AZIMUTH ] ) * kPi / 180.0f;
	const float lamp[ 3 ] = { std::cos( elevation ) * std::cos( azimuth ), std::cos( elevation ) * std::sin( azimuth ),
		                      std::sin( elevation ) };
	//How far a shadow can reach: the tallest junction (twice, for layers that
	//stack) over the lamp's slope, capped where it stops mattering.
	const float reach = std::min( 2.0f * tallest / std::max( std::tan( elevation ), 0.05f ) + 0.002f, 0.6f );

	glBindFramebuffer( GL_FRAMEBUFFER, pgl->HostFBO );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
	{
		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( source );
		ScopedShaderBinding shader( compositeShader.GetGLID() );
		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding inputTexture( source.Handle );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding sheetTexture( sheet.TextureID() );
		ScopedSamplerActivation sampler2( 2 );
		Scoped2DTextureBinding stretchTexture( stretch[ stretchIndex ].TextureID() );

		compositeShader.Set( "InputTexture", 0 );
		compositeShader.Set( "SheetTexture", 1 );
		compositeShader.Set( "StretchTexture", 2 );
		compositeShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		compositeShader.Set( "HalfTexel", 0.5f / static_cast< float >( width ), 0.5f / static_cast< float >( height ) );
		compositeShader.Set( "FrameToGrid", 0.5f, 0.5f );
		compositeShader.Set( "Aspect", aspect );
		compositeShader.Set( "Stretch", StretchFromParam( params[ PT_STRETCH ] ) );
		compositeShader.Set( "Lamp", lamp[ 0 ], lamp[ 1 ], lamp[ 2 ] );
		compositeShader.Set( "Ambient", std::clamp( params[ PT_AMBIENT ], 0.0f, 1.0f ) );
		compositeShader.Set( "Shadows", std::clamp( params[ PT_SHADOWS ], 0.0f, 1.0f ) );
		compositeShader.Set( "ShadowReach", reach );
		//Bounded both ways, so a long reach cannot become a slider-driven
		//hang and a short one is still a march.
		compositeShader.Set( "ShadowSteps", ShadowSteps( reach, height ) );
		compositeShader.Set( "Sheen", SheenFromParam( params[ PT_SHEEN ] ) );
		compositeShader.Set( "Wear", std::clamp( params[ PT_WEAR ], 0.0f, 1.0f ) );
		compositeShader.Set( "Paper", params[ PT_PAPER_R ], params[ PT_PAPER_G ], params[ PT_PAPER_B ] );
		compositeShader.Set( "View", optionIndex( params[ PT_VIEW ], static_cast< int >( View::Count ) ) );
		compositeShader.Set( "MixAmount", params[ PT_MIX ] );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult CrumplePlugin::DeInitGL()
{
	sheetShader.FreeGLResources();
	strainShader.FreeGLResources();
	sourceShader.FreeGLResources();
	fftShader.FreeGLResources();
	solveShader.FreeGLResources();
	compositeShader.FreeGLResources();
	quad.Release();

	sheet.Destroy();
	coarse.Destroy();
	strain.Destroy();
	for( PassBuffer& buffer : spectrum )
		buffer.Destroy();
	for( PassBuffer& buffer : stretch )
		buffer.Destroy();

	for( GLuint* texture : { &twiddleX, &twiddleY } )
	{
		if( *texture != 0 )
			glDeleteTextures( 1, texture );
		*texture = 0;
	}
	if( sheetVBO != 0 )
		glDeleteBuffers( 1, &sheetVBO );
	if( sheetVAO != 0 )
		glDeleteVertexArrays( 1, &sheetVAO );
	sheetVBO = sheetVAO = 0;
	gridWidth = gridHeight = 0;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
void CrumplePlugin::SetSheetForTest( const std::vector< Facet >& facets )
{
	forced  = facets;
	forcing = !facets.empty();
}

void CrumplePlugin::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}

FFResult CrumplePlugin::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

char* CrumplePlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_TEXT )
	{
		static const std::string text = stoatworks::about::textParam( 0 );
		return const_cast< char* >( text.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult CrumplePlugin::SetTextParameter( unsigned int index, const char* value )
{
	if( index == PT_ABOUT_TEXT )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult CrumplePlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;
	if( index >= PT_ABOUT_TEXT )
		return stoatworks::about::handleParam( index - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;
	params[ index ] = value;
	return FF_SUCCESS;
}

float CrumplePlugin::GetFloatParameter( unsigned int index )
{
	return index < PT_COUNT ? params[ index ] : 0.0f;
}

} // namespace crumple
