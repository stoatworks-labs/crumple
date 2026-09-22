#pragma once

/**
    The host's parameters, and what they mean in physical units.

    Every numeric parameter the host sees is a plain 0..1 float, because
    `SetParamInfo` clamps an `FF_TYPE_STANDARD` default into 0..1 *before*
    `SetParamRange` could widen it. The conversions all live in Controls.cpp.
    Option parameters hold the element value.

    **Every length is a fraction of the frame height.** The sheet is the frame,
    so the same settings crumple the same sheet at 720p and at 4K -- and a
    slope, being a length over a length, is the same number at any raster.
*/

namespace crumple
{
/// Parameter ids. **Append only**: SetParamGroup collapses runs of
/// consecutive same-group ids, and saved compositions store parameters by
/// index.
enum ParamId : unsigned int
{
	// -- Sheet --------------------------------------------------------------
	PT_CRUMPLE = 0,
	PT_FLATTEN,
	PT_SCALE,
	PT_LAYERS,
	PT_RELIEF,
	PT_SEED,
	PT_STRETCH,
	PT_DETAIL,

	// -- Lamp ---------------------------------------------------------------
	PT_LAMP_ELEVATION,
	PT_LAMP_AZIMUTH,
	PT_AMBIENT,
	PT_SHADOWS,
	PT_SHEEN,
	PT_WEAR,
	PT_PAPER_R,
	PT_PAPER_G,
	PT_PAPER_B,

	// -- Audio --------------------------------------------------------------
	PT_AUDIO,
	PT_AUDIO_SCRUNCH,

	// -- Output -------------------------------------------------------------
	PT_VIEW,
	PT_MIX,

	// -- The Stoatworks About block (last, so no saved id shifts) -----------
	PT_ABOUT_TEXT,
	PT_ABOUT_BUTTON_1,
	PT_ABOUT_BUTTON_2,
	PT_ABOUT_BUTTON_3,
	PT_COUNT
};

enum class View
{
	Picture = 0,
	Height,
	Normals,
	Stretch,

	Count
};

/// Grid sizes along the long side of the stretch solve's domain -- the frame
/// mirrored to twice its width and height.
constexpr int kDetailCells[] = { 256, 512, 1024 };
constexpr int kDetailCount   = 3;

/// How far through the crumpling: 0 is a flat sheet, 1 has every junction
/// of every generation risen. Linear.
float CrumpleFromParam( float value );

/// How far it has been smoothed back out: every slope times (1 - this).
/// The crease marks stay. Linear.
float FlattenFromParam( float value );

/// The first generation's junction spacing -- the size of its facets -- as a
/// fraction of the frame height: 0.08 to 0.6, geometrically.
float ScaleFromParam( float value );

/// Fold generations, 1 to 4, each 0.45 times finer than the one before.
/// An option parameter: element i is i + 1 layers.
constexpr int kMaxLayers = 4;

/// A multiplier on every facet's slope: 0 to 2, linearly. 1 is the sheet as
/// generated -- facets tilted by up to about 30 degrees.
float ReliefFromParam( float value );

/// Which sheet: 0 to 99.
int SeedFromParam( float value );

/// How far the print follows the sheet: 0 to 2, linearly. 1 is physical --
/// the print pulled in by exactly what an inextensible sheet needs -- and 0
/// leaves the print where it was printed, which is what a texture overlay
/// would do.
float StretchFromParam( float value );

/// Lamp elevation, 5 to 90 degrees, linearly.
float LampElevationFromParam( float value );

/// Lamp azimuth, 0 to 360 degrees, linearly. 0 is a lamp to the right.
float LampAzimuthFromParam( float value );

/// Sheen: a Blinn highlight's strength, 0 to 0.6, linearly.
float SheenFromParam( float value );

/// Audio Scrunch: how much the level (and a kick) add to Crumple, 0 to 1.
float ScrunchFromParam( float value );

} // namespace crumple
