#ifndef HALF_H
#define HALF_H

#include <stdint.h>

uint32_t half_to_float( uint16_t h );
uint16_t half_from_float( uint32_t f );
uint16_t half_add( uint16_t arg0, uint16_t arg1 );
uint16_t half_mul( uint16_t arg0, uint16_t arg1 );

static inline uint16_t 
half_sub( uint16_t ha, uint16_t hb ) 
{
  // (a-b) is the same as (a+(-b))
  return half_add( ha, hb ^ 0x8000 );
}


// Added for NifSkope - BEGIN

inline float halfToFloat( uint16_t u )
{
	union { float f; uint32_t i; } hv;
	hv.i = half_to_float( u );
	return hv.f;
}

inline uint16_t floatToHalf( float f )
{
	union { float f; uint32_t i; } hv;
	hv.f = f;
	return half_from_float( hv.i );
}

// Added for NifSkope - END

#endif /* HALF_H */
