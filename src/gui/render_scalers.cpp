/*
 *  Copyright (C) 2002-2010  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */


//TODO:
//Maybe just do the cache checking back into the simple scalers so they can 
//just handle it all in one go, but this seems to work well enough for now

#include "dosbox.h"
#include "render.h"
#include <string.h>

Bit8u Scaler_Aspect[SCALER_MAXHEIGHT];
Bit16u Scaler_ChangedLines[SCALER_MAXHEIGHT];
Bitu Scaler_ChangedLineIndex;

static union {
	Bit32u b32 [4][SCALER_MAXWIDTH*3];
	Bit16u b16 [4][SCALER_MAXWIDTH*3];
	Bit8u b8 [4][SCALER_MAXWIDTH*3];
} scalerWriteCache;
//scalerFrameCache_t scalerFrameCache;
scalerSourceCache_t scalerSourceCache;

#define _conc2(A,B) A ## B
#define _conc3(A,B,C) A ## B ## C
#define _conc4(A,B,C,D) A ## B ## C ## D
#define _conc5(A,B,C,D,E) A ## B ## C ## D ## E
#define _conc7(A,B,C,D,E,F,G) A ## B ## C ## D ## E ## F ## G

#define conc2(A,B) _conc2(A,B)
#define conc3(A,B,C) _conc3(A,B,C)
#define conc4(A,B,C,D) _conc4(A,B,C,D)
#define conc2d(A,B) _conc3(A,_,B)
#define conc3d(A,B,C) _conc5(A,_,B,_,C)
#define conc4d(A,B,C,D) _conc7(A,_,B,_,C,_,D)

static INLINE void BituMove( void *_dst, const void * _src, Bitu size) {
	Bitu * dst=(Bitu *)(_dst);
	const Bitu * src=(Bitu *)(_src);
	size/=sizeof(Bitu);
	for (Bitu x=0; x<size;x++)
		dst[x] = src[x];
}

static INLINE void ScalerAddLines( Bitu changed, Bitu count ) {
	if ((Scaler_ChangedLineIndex & 1) == changed ) {
		Scaler_ChangedLines[Scaler_ChangedLineIndex] += count;
	} else {
		Scaler_ChangedLines[++Scaler_ChangedLineIndex] = count;
	}
	render.scale.outWrite += render.scale.outPitch * count;
}


#define BituMove2(_DST,_SRC,_SIZE)			\
{											\
	Bitu bsize=(_SIZE)/sizeof(Bitu);		\
	Bitu * bdst=(Bitu *)(_DST);				\
	Bitu * bsrc=(Bitu *)(_SRC);				\
	while (bsize--) *bdst++=*bsrc++;		\
}

#define interp_w2(P0,P1,W0,W1)															\
	((((P0&redblueMask)*W0+(P1&redblueMask)*W1)/(W0+W1)) & redblueMask) |	\
	((((P0&  greenMask)*W0+(P1&  greenMask)*W1)/(W0+W1)) & greenMask)
#define interp_w3(P0,P1,P2,W0,W1,W2)														\
	((((P0&redblueMask)*W0+(P1&redblueMask)*W1+(P2&redblueMask)*W2)/(W0+W1+W2)) & redblueMask) |	\
	((((P0&  greenMask)*W0+(P1&  greenMask)*W1+(P2&  greenMask)*W2)/(W0+W1+W2)) & greenMask)
#define interp_w4(P0,P1,P2,P3,W0,W1,W2,W3)														\
	((((P0&redblueMask)*W0+(P1&redblueMask)*W1+(P2&redblueMask)*W2+(P3&redblueMask)*W3)/(W0+W1+W2+W3)) & redblueMask) |	\
	((((P0&  greenMask)*W0+(P1&  greenMask)*W1+(P2&  greenMask)*W2+(P3&  greenMask)*W3)/(W0+W1+W2+W3)) & greenMask)


#define CC scalerChangeCache

/* Include the different rendering routines */
#define SBPP 8
#define DBPP 8
#include "render_templates.h"
#undef DBPP
#define DBPP 15
#include "render_templates.h"
#undef DBPP
#define DBPP 16
#include "render_templates.h"
#undef DBPP
#define DBPP 32
#include "render_templates.h"
#undef SBPP
#undef DBPP

/* SBPP 9 is a special case with palette check support */
#define SBPP 9
#define DBPP 8
#include "render_templates.h"
#undef DBPP
#define DBPP 15
#include "render_templates.h"
#undef DBPP
#define DBPP 16
#include "render_templates.h"
#undef DBPP
#define DBPP 32
#include "render_templates.h"
#undef SBPP
#undef DBPP

#define SBPP 15
#define DBPP 15
#include "render_templates.h"
#undef DBPP
#define DBPP 16
#include "render_templates.h"
#undef DBPP
#define DBPP 32
#include "render_templates.h"
#undef SBPP
#undef DBPP

#define SBPP 16
#define DBPP 15
#include "render_templates.h"
#undef DBPP
#define DBPP 16
#include "render_templates.h"
#undef DBPP
#define DBPP 32
#include "render_templates.h"
#undef SBPP
#undef DBPP

#define SBPP 32
#define DBPP 15
#include "render_templates.h"
#undef DBPP
#define DBPP 16
#include "render_templates.h"
#undef DBPP
#define DBPP 32
#include "render_templates.h"
#undef SBPP
#undef DBPP


ScalerSimpleBlock_t ScaleNormal1x = {
	"Normal",
	GFX_CAN_8|GFX_CAN_15|GFX_CAN_16|GFX_CAN_32,
	1,1,{
{	Normal1x_8_8_L,		Normal1x_8_15_L ,	Normal1x_8_16_L ,	Normal1x_8_32_L },
{	             0,		Normal1x_15_15_L,	Normal1x_15_16_L,	Normal1x_15_32_L},
{	             0,		Normal1x_16_15_L,	Normal1x_16_16_L,	Normal1x_16_32_L},
{	             0,		Normal1x_32_15_L,	Normal1x_32_16_L,	Normal1x_32_32_L},
{	Normal1x_8_8_L,		Normal1x_9_15_L ,	Normal1x_9_16_L ,	Normal1x_9_32_L }
},{
{	Normal1x_8_8_R,		Normal1x_8_15_R ,	Normal1x_8_16_R ,	Normal1x_8_32_R },
{	             0,		Normal1x_15_15_R,	Normal1x_15_16_R,	Normal1x_15_32_R},
{	             0,		Normal1x_16_15_R,	Normal1x_16_16_R,	Normal1x_16_32_R},
{	             0,		Normal1x_32_15_R,	Normal1x_32_16_R,	Normal1x_32_32_R},
{	Normal1x_8_8_R,		Normal1x_9_15_R ,	Normal1x_9_16_R ,	Normal1x_9_32_R }
}};

ScalerSimpleBlock_t ScaleNormalDw = {
	"Normal",
	GFX_CAN_8|GFX_CAN_15|GFX_CAN_16|GFX_CAN_32,
	2,1,{
{	NormalDw_8_8_L,		NormalDw_8_15_L ,	NormalDw_8_16_L ,	NormalDw_8_32_L },
{	             0,		NormalDw_15_15_L,	NormalDw_15_16_L,	NormalDw_15_32_L},
{	             0,		NormalDw_16_15_L,	NormalDw_16_16_L,	NormalDw_16_32_L},
{	             0,		NormalDw_32_15_L,	NormalDw_32_16_L,	NormalDw_32_32_L},
{	NormalDw_8_8_L,		NormalDw_9_15_L ,	NormalDw_9_16_L ,	NormalDw_9_32_L }
},{
{	NormalDw_8_8_R,		NormalDw_8_15_R ,	NormalDw_8_16_R ,	NormalDw_8_32_R },
{	             0,		NormalDw_15_15_R,	NormalDw_15_16_R,	NormalDw_15_32_R},
{	             0,		NormalDw_16_15_R,	NormalDw_16_16_R,	NormalDw_16_32_R},
{	             0,		NormalDw_32_15_R,	NormalDw_32_16_R,	NormalDw_32_32_R},
{	NormalDw_8_8_R,		NormalDw_9_15_R ,	NormalDw_9_16_R ,	NormalDw_9_32_R }
}};

ScalerSimpleBlock_t ScaleNormalDh = {
	"Normal",
	GFX_CAN_8|GFX_CAN_15|GFX_CAN_16|GFX_CAN_32,
	1,2,{
{	NormalDh_8_8_L,		NormalDh_8_15_L ,	NormalDh_8_16_L ,	NormalDh_8_32_L },
{	             0,		NormalDh_15_15_L,	NormalDh_15_16_L,	NormalDh_15_32_L},
{	             0,		NormalDh_16_15_L,	NormalDh_16_16_L,	NormalDh_16_32_L},
{	             0,		NormalDh_32_15_L,	NormalDh_32_16_L,	NormalDh_32_32_L},
{	NormalDh_8_8_L,		NormalDh_9_15_L ,	NormalDh_9_16_L ,	NormalDh_9_32_L }
},{
{	NormalDh_8_8_R,		NormalDh_8_15_R ,	NormalDh_8_16_R ,	NormalDh_8_32_R },
{	             0,		NormalDh_15_15_R,	NormalDh_15_16_R,	NormalDh_15_32_R},
{	             0,		NormalDh_16_15_R,	NormalDh_16_16_R,	NormalDh_16_32_R},
{	             0,		NormalDh_32_15_R,	NormalDh_32_16_R,	NormalDh_32_32_R},
{	NormalDh_8_8_R,		NormalDh_9_15_R ,	NormalDh_9_16_R ,	NormalDh_9_32_R }
}};

