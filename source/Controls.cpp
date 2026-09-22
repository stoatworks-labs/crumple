#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace crumple
{
namespace
{
float geometric( float value, float low, float high )
{
	return low * std::pow( high / low, std::clamp( value, 0.0f, 1.0f ) );
}

float linear( float value, float low, float high )
{
	return low + ( high - low ) * std::clamp( value, 0.0f, 1.0f );
}
} // namespace

float CrumpleFromParam( float value )
{
	return std::clamp( value, 0.0f, 1.0f );
}

float FlattenFromParam( float value )
{
	return std::clamp( value, 0.0f, 1.0f );
}

float ScaleFromParam( float value )
{
	return geometric( value, 0.08f, 0.6f );
}

float ReliefFromParam( float value )
{
	return linear( value, 0.0f, 2.0f );
}

int SeedFromParam( float value )
{
	return std::clamp( static_cast< int >( std::floor( std::clamp( value, 0.0f, 1.0f ) * 99.999f ) ), 0, 99 );
}

float StretchFromParam( float value )
{
	return linear( value, 0.0f, 2.0f );
}

float LampElevationFromParam( float value )
{
	return linear( value, 5.0f, 90.0f );
}

float LampAzimuthFromParam( float value )
{
	return linear( value, 0.0f, 360.0f );
}

float SheenFromParam( float value )
{
	return linear( value, 0.0f, 0.6f );
}

float ScrunchFromParam( float value )
{
	return std::clamp( value, 0.0f, 1.0f );
}

} // namespace crumple
