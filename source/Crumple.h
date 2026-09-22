#pragma once

#include "Audio.h"
#include "Controls.h"
#include "PassBuffer.h"
#include "Sheet.h"
#include "Shaders.h"

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

#include <vector>

namespace crumple
{
/// How many steps the shadow march takes for this reach and picture. The
/// harness derives its tolerance from the same number.
int ShadowSteps( float reach, int pictureHeight );

/**
    The plugin. The sheet (passes 1-2), the stretch it forces on the print
    (passes 3-6), and the lamp (pass 7). See AGENTS.md for each.
*/
class CrumplePlugin : public CFFGLPlugin
{
public:
	CrumplePlugin();

	FFResult InitGL( const FFGLViewportStruct* viewport ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* input ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default: without
	/// it no real host can instantiate the plugin, while every offline
	/// harness carries on passing.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	FFResult SetTime( double time ) override;

	//-------------------------------------------------------------------
	// For the harness.
	//-------------------------------------------------------------------
	void SetClockScaleForTest( double scale );

	/// Replace the generated sheet with these facets, drawn as given
	/// (Crumple, Flatten and Relief no longer apply), so a check can put one
	/// crease of known slope exactly where it wants it. An empty list goes
	/// back to the generator.
	void SetSheetForTest( const std::vector< Facet >& facets );

	/// The stretch field ( u_x, u_y ) on the solve's grid, frame-height units.
	GLuint StretchTextureID() const
	{
		return stretch[ stretchIndex ].TextureID();
	}
	int GridWidth() const
	{
		return gridWidth;
	}
	int GridHeight() const
	{
		return gridHeight;
	}

	/// The facets the last frame drew.
	const std::vector< Facet >& DrawnSheet() const
	{
		return drawn;
	}

private:
	void UpdateClock();
	bool ensureBuffers( GLsizei width, GLsizei height, int nx, int ny );
	void DrawSheet( PassBuffer& target, GLsizei vertexCount, float aspect, float markWidth );
	void Transform( PassBuffer ( &buffers )[ 2 ], int& current, float direction );
	static GLuint MakeTwiddles( int length );

	float params[ PT_COUNT ] = {};

	ffglex::FFGLShader sheetShader;
	ffglex::FFGLShader strainShader;
	ffglex::FFGLShader sourceShader;
	ffglex::FFGLShader fftShader;
	ffglex::FFGLShader solveShader;
	ffglex::FFGLShader compositeShader;
	ffglex::FFGLScreenQuad quad;

	PassBuffer sheet;        ///< RGBA32F picture size: h, h_x, h_y, marks
	PassBuffer coarse;       ///< the same, at four samples a grid cell: for the strain
	PassBuffer strain;       ///< RGBA32F, coarse size, mipmapped
	PassBuffer spectrum[ 2 ];///< RGBA32F grid: the forward transform
	PassBuffer stretch[ 2 ]; ///< RGBA32F grid: the solve and the inverse; u
	int stretchIndex = 0;
	int gridWidth    = 0;
	int gridHeight   = 0;

	GLuint sheetVAO = 0;
	GLuint sheetVBO = 0;
	GLuint twiddleX = 0;
	GLuint twiddleY = 0;

	/// The triangulated layers, kept until the seed, scale, layer count or
	/// aspect moves -- the triangulation is the only slow part of a sheet.
	std::vector< Layer > layers;
	SheetSettings layersFor;
	bool haveLayers = false;

	std::vector< Facet > drawn;
	std::vector< Facet > forced;
	bool forcing = false;

	/// What the sheet, the strain and the stretch were last built for. A
	/// sheet nobody is moving -- the usual case -- is built once, and every
	/// frame after is the composite alone.
	SheetSettings builtFor;
	GLsizei builtWidth  = 0;
	GLsizei builtHeight = 0;
	int builtGrid       = 0;
	bool built          = false;
	float tallest       = 0.0f;

	//-------------------------------------------------------------------
	// Time, for the audio envelope only. Rosette's unit vote.
	//-------------------------------------------------------------------
	double hostTime     = -1.0;
	double lastRawTime  = -1.0;
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	double clockScale   = 0.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double now          = 0.0;
	double lastNow      = -1.0;

	audio::Analyser analyser;
};

} // namespace crumple
