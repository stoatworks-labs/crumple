#include "Sheet.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace crumple
{
namespace
{
uint32_t lowbias32( uint32_t x )
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

/// A uniform number in [0, 1) for ( seed, layer, index, which ). Integer only.
double draw( int seed, int layer, int index, int which )
{
	uint32_t h = lowbias32( static_cast< uint32_t >( seed ) * 0x9e3779b9U + 0x632be5abU );
	h          = lowbias32( h ^ ( static_cast< uint32_t >( layer ) * 0x85ebca6bU ) );
	h          = lowbias32( h ^ ( static_cast< uint32_t >( index ) * 0xc2b2ae35U ) );
	h          = lowbias32( h ^ ( static_cast< uint32_t >( which ) + 0x27d4eb2fU ) );
	return static_cast< double >( h ) / 4294967296.0;
}

/// How far past the frame, beyond one cell, the junctions reach.
constexpr float kMargin = 0.02f;

struct Triangle
{
	int a, b, c;
	double cx, cy, r2;//circumcircle
};

bool circumcircle( const std::vector< double >& px, const std::vector< double >& py, Triangle& t )
{
	const double ax = px[ t.a ], ay = py[ t.a ];
	const double bx = px[ t.b ], by = py[ t.b ];
	const double cx = px[ t.c ], cy = py[ t.c ];
	const double d  = 2.0 * ( ax * ( by - cy ) + bx * ( cy - ay ) + cx * ( ay - by ) );
	if( std::fabs( d ) < 1e-18 )
		return false;
	const double a2 = ax * ax + ay * ay, b2 = bx * bx + by * by, c2 = cx * cx + cy * cy;
	t.cx            = ( a2 * ( by - cy ) + b2 * ( cy - ay ) + c2 * ( ay - by ) ) / d;
	t.cy            = ( a2 * ( cx - bx ) + b2 * ( ax - cx ) + c2 * ( bx - ax ) ) / d;
	t.r2            = ( ax - t.cx ) * ( ax - t.cx ) + ( ay - t.cy ) * ( ay - t.cy );
	return true;
}

void normalOf( const Facet& f, double n[ 3 ] )
{
	const double ux = f.x[ 1 ] - f.x[ 0 ], uy = f.y[ 1 ] - f.y[ 0 ], uz = f.z[ 1 ] - f.z[ 0 ];
	const double vx = f.x[ 2 ] - f.x[ 0 ], vy = f.y[ 2 ] - f.y[ 0 ], vz = f.z[ 2 ] - f.z[ 0 ];
	n[ 0 ]          = uy * vz - uz * vy;
	n[ 1 ]          = uz * vx - ux * vz;
	n[ 2 ]          = ux * vy - uy * vx;
	const double l  = std::sqrt( n[ 0 ] * n[ 0 ] + n[ 1 ] * n[ 1 ] + n[ 2 ] * n[ 2 ] );
	for( int i = 0; i < 3; ++i )
		n[ i ] /= std::max( l, 1e-30 );
}

/// Which facets share which edges. An edge on the outside of the
/// triangulation has one facet, and no crease.
std::vector< Layer::Shared > findShared( const std::vector< int >& corners )
{
	std::map< std::pair< int, int >, std::vector< std::pair< int, int > > > edges;
	for( size_t t = 0; t < corners.size() / 3; ++t )
		for( int k = 0; k < 3; ++k )
		{
			const int a = corners[ t * 3 + ( k + 1 ) % 3 ], b = corners[ t * 3 + ( k + 2 ) % 3 ];
			edges[ { std::min( a, b ), std::max( a, b ) } ].push_back( { static_cast< int >( t ), k } );
		}
	std::vector< Layer::Shared > shared;
	for( const auto& entry : edges )
		if( entry.second.size() == 2 )
			shared.push_back( { entry.second[ 0 ].first, entry.second[ 0 ].second, entry.second[ 1 ].first,
			                    entry.second[ 1 ].second } );
	return shared;
}

/// Each edge's crease: the angle between the two facets that share it.
void measureCreases( std::vector< Facet >& facets, const std::vector< Layer::Shared >& shared )
{
	for( const Layer::Shared& e : shared )
	{
		double n0[ 3 ], n1[ 3 ];
		normalOf( facets[ static_cast< size_t >( e.facetA ) ], n0 );
		normalOf( facets[ static_cast< size_t >( e.facetB ) ], n1 );
		const float angle = static_cast< float >(
			std::acos( std::clamp( n0[ 0 ] * n1[ 0 ] + n0[ 1 ] * n1[ 1 ] + n0[ 2 ] * n1[ 2 ], -1.0, 1.0 ) ) );
		facets[ static_cast< size_t >( e.facetA ) ].crease[ e.edgeA ] = angle;
		facets[ static_cast< size_t >( e.facetB ) ].crease[ e.edgeB ] = angle;
	}
}
} // namespace

//---------------------------------------------------------------------------
std::vector< std::vector< Junction > > BuildJunctions( const SheetSettings& s )
{
	std::vector< std::vector< Junction > > layers;
	const int count = std::clamp( s.layers, 1, 4 );

	for( int layer = 0; layer < count; ++layer )
	{
		std::vector< Junction > junctions;

		//Stratified: one junction in each cell of a grid at this layer's
		//spacing, somewhere inside it. Evenly spread facets, none of them
		//slivers, and a count that follows the spacing exactly.
		float spacing = s.scale * std::pow( kLayerRatio, static_cast< float >( layer ) );
		bool capped   = false;
		//The cap depends on the area, and the area on the spacing (the margin
		//is a cell wide): iterate to the fixed point, which a few steps reach
		//because the margin is a small part of the area.
		for( int pass = 0; pass < 8; ++pass )
		{
			const float reach = spacing + kMargin;
			const float area  = ( s.aspect + 2.0f * reach ) * ( 1.0f + 2.0f * reach );
			if( area / ( spacing * spacing ) <= kMaxJunctions * 0.98f )
				break;
			spacing = std::sqrt( area / ( kMaxJunctions * 0.98f ) );
			capped  = true;
		}

		//The margin is a whole cell past the frame and a little more, so the
		//outermost ring of junctions is entirely outside it. The corner cells'
		//junctions then lie diagonally beyond the frame's corners, and the
		//triangulation -- their convex hull -- covers the frame completely.
		const float reach = spacing + kMargin;
		const float left  = -reach, right = s.aspect + reach;
		const float below = -reach, above = 1.0f + reach;
		const int columns = std::max( 2, static_cast< int >( std::ceil( ( right - left ) / spacing ) ) );
		const int rows    = std::max( 2, static_cast< int >( std::ceil( ( above - below ) / spacing ) ) );

		//The generations arrive in turn: the first at once, each later one
		//starting a little further in, all of them finished by Crumple 0.85
		//+ the forming span, i.e. exactly at 1.
		const float start = count > 1 ? 0.55f * static_cast< float >( layer ) / static_cast< float >( count - 1 ) : 0.0f;

		for( int j = 0; j < rows; ++j )
			for( int i = 0; i < columns; ++i )
			{
				const int index = j * columns + i;
				Junction p;
				p.x = left + ( i + static_cast< float >( draw( s.seed, layer, index, 0 ) ) ) * ( right - left ) / columns;
				p.y = below + ( j + static_cast< float >( draw( s.seed, layer, index, 1 ) ) ) * ( above - below ) / rows;
				//Up or down by up to the steepness times the spacing: every
				//facet has a slope of the same order at every generation.
				p.height  = static_cast< float >( ( 2.0 * draw( s.seed, layer, index, 2 ) - 1.0 ) * kSteepness * spacing );
				p.arrival = start + 0.3f * static_cast< float >( draw( s.seed, layer, index, 3 ) );
				junctions.push_back( p );
			}
		layers.push_back( std::move( junctions ) );
		if( capped )
			break;
	}
	return layers;
}

float Formed( const Junction& j, float crumple )
{
	const float x = std::clamp( ( crumple - j.arrival ) / kFormingSpan, 0.0f, 1.0f );
	return x * x * ( 3.0f - 2.0f * x );
}

//---------------------------------------------------------------------------
std::vector< int > Triangulate( const std::vector< Junction >& points )
{
	//Bowyer-Watson, in double. O(n^2), which at the sizes a sheet has (a
	//thousand junctions at most) is a millisecond, and it only runs when a
	//sheet control changes.
	const int n = static_cast< int >( points.size() );
	std::vector< double > px( n + 3 ), py( n + 3 );
	double lo[ 2 ] = { 1e30, 1e30 }, hi[ 2 ] = { -1e30, -1e30 };
	for( int i = 0; i < n; ++i )
	{
		px[ i ] = points[ i ].x;
		py[ i ] = points[ i ].y;
		lo[ 0 ] = std::min( lo[ 0 ], px[ i ] );
		lo[ 1 ] = std::min( lo[ 1 ], py[ i ] );
		hi[ 0 ] = std::max( hi[ 0 ], px[ i ] );
		hi[ 1 ] = std::max( hi[ 1 ], py[ i ] );
	}
	const double span = std::max( hi[ 0 ] - lo[ 0 ], hi[ 1 ] - lo[ 1 ] ) + 1.0;
	const double mx = 0.5 * ( lo[ 0 ] + hi[ 0 ] ), my = 0.5 * ( lo[ 1 ] + hi[ 1 ] );
	px[ n ]     = mx - 20.0 * span;
	py[ n ]     = my - span;
	px[ n + 1 ] = mx;
	py[ n + 1 ] = my + 20.0 * span;
	px[ n + 2 ] = mx + 20.0 * span;
	py[ n + 2 ] = my - span;

	std::vector< Triangle > triangles;
	Triangle super { n, n + 1, n + 2, 0, 0, 0 };
	circumcircle( px, py, super );
	triangles.push_back( super );

	for( int p = 0; p < n; ++p )
	{
		std::vector< std::pair< int, int > > boundary;
		std::vector< Triangle > kept;
		kept.reserve( triangles.size() );
		for( const Triangle& t : triangles )
		{
			const double dx = px[ p ] - t.cx, dy = py[ p ] - t.cy;
			if( dx * dx + dy * dy < t.r2 )
			{
				const std::pair< int, int > sides[ 3 ] = { { t.a, t.b }, { t.b, t.c }, { t.c, t.a } };
				for( const auto& e : sides )
				{
					//An edge shared by two doomed triangles is interior to the
					//hole; one seen once is its boundary.
					auto twin = std::find_if( boundary.begin(), boundary.end(), [ & ]( const std::pair< int, int >& o ) {
						return o.first == e.second && o.second == e.first;
					} );
					if( twin != boundary.end() )
						boundary.erase( twin );
					else
						boundary.push_back( e );
				}
			}
			else
				kept.push_back( t );
		}
		for( const auto& e : boundary )
		{
			Triangle t { e.first, e.second, p, 0, 0, 0 };
			if( circumcircle( px, py, t ) )
				kept.push_back( t );
		}
		triangles.swap( kept );
	}

	std::vector< int > out;
	for( const Triangle& t : triangles )
	{
		if( t.a >= n || t.b >= n || t.c >= n )
			continue;
		//Counter-clockwise, so every facet's normal points out of the page.
		const double cross = ( px[ t.b ] - px[ t.a ] ) * ( py[ t.c ] - py[ t.a ] )
		                     - ( py[ t.b ] - py[ t.a ] ) * ( px[ t.c ] - px[ t.a ] );
		out.push_back( t.a );
		out.push_back( cross > 0 ? t.b : t.c );
		out.push_back( cross > 0 ? t.c : t.b );
	}
	return out;
}

//---------------------------------------------------------------------------
std::vector< Layer > BuildLayers( const SheetSettings& s )
{
	std::vector< Layer > layers;
	for( std::vector< Junction >& junctions : BuildJunctions( s ) )
	{
		Layer layer;
		layer.corners   = Triangulate( junctions );
		layer.shared    = findShared( layer.corners );
		layer.junctions = std::move( junctions );
		layers.push_back( std::move( layer ) );
	}
	return layers;
}

std::vector< Facet > FacetsFrom( const std::vector< Layer >& layers, const SheetSettings& s )
{
	std::vector< Facet > sheet;
	const float flatten = std::clamp( s.flatten, 0.0f, 1.0f );

	for( const Layer& layer : layers )
	{
		const size_t count = layer.corners.size() / 3;
		std::vector< Facet > crumpled( count );
		for( size_t t = 0; t < count; ++t )
			for( int k = 0; k < 3; ++k )
			{
				const Junction& j      = layer.junctions[ static_cast< size_t >( layer.corners[ t * 3 + k ] ) ];
				crumpled[ t ].x[ k ] = j.x;
				crumpled[ t ].y[ k ] = j.y;
				crumpled[ t ].z[ k ] = j.height * Formed( j, s.crumple ) * s.relief;
				crumpled[ t ].layer  = static_cast< int >( &layer - layers.data() );
			}

		//The creases are measured on the sheet as crumpled, BEFORE it is
		//flattened: smoothing a sheet out does not unbreak its fibres.
		measureCreases( crumpled, layer.shared );
		for( Facet& f : crumpled )
			for( float& z : f.z )
				z *= 1.0f - flatten;

		sheet.insert( sheet.end(), crumpled.begin(), crumpled.end() );
	}
	return sheet;
}

std::vector< Facet > BuildSheet( const SheetSettings& s )
{
	return FacetsFrom( BuildLayers( s ), s );
}

//---------------------------------------------------------------------------
std::vector< Facet > RidgeSheet( float x0, float halfWidth, float slope, float aspect )
{
	const float xs[] = { -0.5f, x0 - halfWidth, x0, x0 + halfWidth, aspect + 0.5f };
	const float zs[] = { 0.0f, 0.0f, slope * halfWidth, 0.0f, 0.0f };
	const float y0 = -1.0f, y1 = 2.0f;

	std::vector< Facet > sheet;
	for( int i = 0; i < 4; ++i )
	{
		Facet lower, upper;
		//Two triangles per strip, counter-clockwise.
		const float ax = xs[ i ], bx = xs[ i + 1 ], az = zs[ i ], bz = zs[ i + 1 ];
		lower = Facet { { ax, bx, bx }, { y0, y0, y1 }, { az, bz, bz }, {} };
		upper = Facet { { ax, bx, ax }, { y0, y1, y1 }, { az, bz, az }, {} };
		sheet.push_back( lower );
		sheet.push_back( upper );
	}
	return sheet;
}

} // namespace crumple
