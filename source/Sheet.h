#pragma once

#include <cstdint>
#include <vector>

/**
    The sheet: a crumpled piece of paper as a sum of piecewise-planar layers.

    Crumpled paper is facets and creases. Almost every point of it is tilted,
    each facet is very nearly flat -- paper will bend in one direction without
    stretching, not two -- and the facets meet along straight creases that
    run between junctions where three or more of them meet. A random
    triangulated surface is exactly that: scatter the junctions, join them by
    a Delaunay triangulation, give each junction a height, and every triangle
    is a flat facet and every edge a straight crease.

    A hand crumples in generations: a few big folds first, then smaller ones
    inside them. So the sheet is up to four LAYERS, each a triangulated
    surface 0.45 times finer than the one before and arriving later, and the
    height is their SUM -- which is still piecewise planar, on the overlay of
    the triangulations.

    Junctions are drawn from an integer hash of ( seed, layer, index ), so a
    sheet is the same sheet on every machine and at every Crumple, and the
    triangulation depends only on where the junctions are, never on how far
    they have risen. Raising Crumple only ever raises heights towards their
    final value: `crtest --monotone` holds that.
*/
namespace crumple
{

/// One flat facet: three corners in frame-height units (x right, y up, z out
/// of the page) and, for each edge, how sharp the crease there is -- the
/// angle between this facet and its neighbour across it. Edge k is opposite
/// corner k.
struct Facet
{
	float x[ 3 ] = {};
	float y[ 3 ] = {};
	float z[ 3 ] = {};
	float crease[ 3 ] = {};
	int layer         = 0;///< which generation: the layers ADD, so their ranges do too
};

struct SheetSettings
{
	int seed        = 0;
	int layers      = 3;    ///< fold generations, 1 to 4
	float scale     = 0.3f; ///< the first generation's junction spacing, frame heights
	float relief    = 1.0f; ///< slope multiplier
	float crumple   = 1.0f; ///< how far through the crumpling, 0..1
	float flatten   = 0.0f; ///< every height times ( 1 - this )
	float aspect    = 16.0f / 9.0f;
};

/// Each generation is this much finer than the one before.
constexpr float kLayerRatio = 0.45f;

/// A facet's typical slope before Relief: its junctions rise by up to this
/// times the junction spacing.
constexpr float kSteepness = 0.55f;

/// The most junctions one layer may have. A 0.6-frame first generation with
/// four layers asks for about 1,100 in its finest; Scale at the bottom of its
/// travel would ask for far more. The layer that hits this is built at the
/// spacing the cap allows and is the LAST: any finer generation would be
/// capped to the same spacing and lie on top of it, three copies of one
/// layer rather than three generations.
constexpr int kMaxJunctions = 1600;

/// A junction: where it is, and the height it will reach when fully formed.
struct Junction
{
	float x = 0.0f, y = 0.0f;
	float height  = 0.0f;
	float arrival = 0.0f;///< the Crumple at which it starts to rise
};

/// Every junction of every layer, in layer order, with their arrivals.
/// Independent of Crumple and Flatten: those only scale the heights.
std::vector< std::vector< Junction > > BuildJunctions( const SheetSettings& settings );

/// How far a junction has risen at this Crumple: 0 before its arrival,
/// smoothly to 1 over the next kFormingSpan. Monotone in `crumple`.
float Formed( const Junction& j, float crumple );
constexpr float kFormingSpan = 0.15f;

/// The Delaunay triangulation of these points, as index triples.
std::vector< int > Triangulate( const std::vector< Junction >& points );

/// One generation: its junctions and their triangulation. Depends only on
/// the seed, the scale, the layer count and the aspect, so the plugin builds
/// it once and keeps it until one of those moves.
struct Layer
{
	std::vector< Junction > junctions;
	std::vector< int > corners;///< index triples, counter-clockwise

	/// Each interior edge once: the two facets that share it and which of
	/// each one's edges it is. Found with the triangulation and kept with it,
	/// so measuring the creases every frame is a walk down a list rather than
	/// a map built from scratch.
	struct Shared
	{
		int facetA, edgeA, facetB, edgeB;
	};
	std::vector< Shared > shared;
};
std::vector< Layer > BuildLayers( const SheetSettings& settings );

/// The sheet as flat facets at this Crumple, Flatten and Relief.
std::vector< Facet > FacetsFrom( const std::vector< Layer >& layers, const SheetSettings& settings );

/// Both of the above, for callers that do not keep the layers.
std::vector< Facet > BuildSheet( const SheetSettings& settings );

/// For the harness: a straight ridge the full height of the sheet, spine at
/// x0, rising from 0 at x0 +- W to s W at the spine, flat everywhere else --
/// one crease of slope s either side, of known everything.
std::vector< Facet > RidgeSheet( float x0, float halfWidth, float slope, float aspect );

} // namespace crumple
