//
// hulltest -- the PC gate for milestone 18 step 4, the collision hull trace.
//
// gpu64_levelMove() is the one piece of the level pipeline that has no
// picture: it answers "can I walk here?" and a wrong answer looks exactly
// like a right one on the HDMI screen until the player is standing outside
// the map. So it gets a test with a verdict instead of a render.
//
// Everything here runs the firmware's own Source/Firmware/gpu64_level.cpp --
// the same rule levelsim follows, and for the same reason: two
// implementations of a hull trace would agree right up until the one at the
// bench was the other one.
//
//   tools/hostsim/hulltest build/e1m1.g64lev
//
// Exit 0 if every check passes. Any failure prints the position it failed at,
// in world units and in Quake units, so it can be found in a map editor.
//
// Copyright (c) 2026 Honza Slesinger
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "gpu64_level.h"

static Gpu64_Level	g_L;
static u8	       *g_File;
static u32		g_FileLen;
static Gpu64_LevelHull	g_World;
static s32		g_Head;		// hull 1, the player hull

static unsigned		g_Fail;

#define WU( x )		( (double)( x ) / 65536.0 )
#define QU( x )		( WU( x ) * (double)g_L.nScale )

static void fail( const char *pWhat, const Gpu64_LevelVec *pP )
{
	g_Fail++;
	printf( "  FAIL  %-34s", pWhat );
	if ( pP )
		printf( "  wu %8.3f %8.3f %8.3f   qu %8.1f %8.1f %8.1f",
			WU( pP->v[ 0 ] ), WU( pP->v[ 1 ] ), WU( pP->v[ 2 ] ),
			QU( pP->v[ 0 ] ), QU( pP->v[ 1 ] ), QU( pP->v[ 2 ] ) );
	printf( "\n" );
}

static boolean load( const char *pPath )
{
	FILE *f = fopen( pPath, "rb" );
	if ( !f ) { fprintf( stderr, "hulltest: cannot open %s\n", pPath ); return FALSE; }
	fseek( f, 0, SEEK_END );
	long n = ftell( f );
	fseek( f, 0, SEEK_SET );
	g_File = (u8 *)malloc( n ? n : 1 );
	if ( fread( g_File, 1, n, f ) != (size_t)n )
	{
		fprintf( stderr, "hulltest: short read on %s\n", pPath );
		fclose( f ); return FALSE;
	}
	fclose( f );
	g_FileLen = (u32)n;
	return gpu64_levelParse( &g_L, g_File, g_FileLen );
}

// --- the checks ----------------------------------------------------------

static boolean inSolid( const Gpu64_LevelVec *pP )
{
	return gpu64_levelPointContents( &g_L, g_Head, pP ) == GPU64_LEVEL_CONTENTS_SOLID;
}

// The world model's own bounding box, with a world unit of slack. Leaving it
// is the failure this whole file exists to catch: outside the map there is no
// geometry, the renderer draws black, and the bench photograph looks like a
// firmware hang.
static boolean inBounds( const Gpu64_LevelVec *pP )
{
	for ( unsigned k = 0; k < 3; k++ )
		if ( pP->v[ k ] < g_World.mins.v[ k ] - 65536 ||
		     pP->v[ k ] > g_World.maxs.v[ k ] + 65536 )
			return FALSE;
	return TRUE;
}

static boolean playerStart( Gpu64_LevelVec *pOut, u16 *pYaw )
{
	for ( unsigned i = 0; i < g_L.nEnt; i++ )
	{
		Gpu64_LevelEnt e;
		if ( !gpu64_levelEnt( &g_L, i, &e ) )
			continue;
		if ( strcmp( e.pClassName, "info_player_start" ) )
			continue;
		pOut->v[ 0 ] = e.x;
		pOut->v[ 1 ] = e.y;
		pOut->v[ 2 ] = e.z;
		*pYaw = e.nYaw;
		return TRUE;
	}
	return FALSE;
}

// One frame of walking: a horizontal step of nSpeed Quake units on the given
// heading, plus a downward pull, resolved as a walk.
static u8 step( Gpu64_LevelVec *pPos, double fHeading, double fSpeedQU, u8 nMode )
{
	const double u = 65536.0 / (double)g_L.nScale;	// one Quake unit, 16.16

	Gpu64_LevelVec d;
	d.v[ 0 ] = (s32)( cos( fHeading ) * fSpeedQU * u );
	d.v[ 1 ] = (s32)( -8.0 * u );			// gravity, a fixed pull
	d.v[ 2 ] = (s32)( sin( fHeading ) * fSpeedQU * u );

	Gpu64_LevelMove mv;
	if ( !gpu64_levelMove( &g_L, 0, 1, nMode, pPos, &d, &mv ) )
	{
		fail( "gpu64_levelMove refused", pPos );
		return 0;
	}
	*pPos = mv.end;
	return mv.nFlags;
}

// --- main ----------------------------------------------------------------

int main( int argc, char **argv )
{
	const char *pPath = ( argc > 1 ) ? argv[ 1 ] : "build/e1m1.g64lev";

	if ( !load( pPath ) )
	{
		fprintf( stderr, "hulltest: %s is not a level this build can parse\n", pPath );
		return 1;
	}

	printf( "hulltest %s\n", pPath );
	printf( "  %u planes  %u hulls  %u clipnodes  scale %u qu/wu\n",
		g_L.nPlane, g_L.nHull, g_L.nClip, g_L.nScale );

	if ( !gpu64_levelHull( &g_L, 0, &g_World ) )
	{
		printf( "  FAIL  no world model in the hull table\n" );
		return 1;
	}
	g_Head = g_World.nHead[ 0 ];
	printf( "  world hull 1 head %d, hull 2 head %d\n", g_World.nHead[ 0 ], g_World.nHead[ 1 ] );
	printf( "  world box qu %.0f %.0f %.0f .. %.0f %.0f %.0f\n",
		QU( g_World.mins.v[0] ), QU( g_World.mins.v[1] ), QU( g_World.mins.v[2] ),
		QU( g_World.maxs.v[0] ), QU( g_World.maxs.v[1] ), QU( g_World.maxs.v[2] ) );

	// --- 1. the spawn point is somewhere a player can be ---------------
	Gpu64_LevelVec spawn;
	u16 nYaw = 0;
	if ( !playerStart( &spawn, &nYaw ) )
	{
		printf( "  FAIL  no info_player_start\n" );
		return 1;
	}
	printf( "  spawn qu %.0f %.0f %.0f  yaw %04x\n",
		QU( spawn.v[0] ), QU( spawn.v[1] ), QU( spawn.v[2] ), nYaw );

	if ( inSolid( &spawn ) )
		fail( "info_player_start is inside solid", &spawn );

	// --- 2. outside the map is solid -----------------------------------
	// Not a formality: if the tree answers EMPTY out here then every test
	// below passes by accident.
	{
		Gpu64_LevelVec far_;
		far_.v[ 0 ] = g_World.maxs.v[ 0 ] + 20 * 65536;
		far_.v[ 1 ] = g_World.maxs.v[ 1 ] + 20 * 65536;
		far_.v[ 2 ] = g_World.maxs.v[ 2 ] + 20 * 65536;
		if ( !inSolid( &far_ ) )
			fail( "outside the map is not solid", &far_ );
	}

	// --- 3. a trace that hits nothing goes the whole way ---------------
	{
		Gpu64_LevelTrace tr;
		Gpu64_LevelVec a = spawn, b = spawn;
		b.v[ 1 ] += 65536 / 64;			// a sixteenth of a Quake unit up
		gpu64_levelTrace( &g_L, g_Head, &a, &b, &tr );
		if ( tr.nFraction != 0x10000 )
			fail( "a trace through open air was clipped", &spawn );
		if ( tr.bAllSolid || tr.bStartSolid )
			fail( "open air reports solid", &spawn );
	}

	// --- 4. the floor is under the spawn, and it is reachable ----------
	{
		Gpu64_LevelTrace tr;
		Gpu64_LevelVec a = spawn, b = spawn;
		b.v[ 1 ] -= 64 * 65536 / (s32)g_L.nScale * 8;	// 64 Quake units down
		gpu64_levelTrace( &g_L, g_Head, &a, &b, &tr );
		if ( tr.nFraction >= 0x10000 )
			fail( "no floor within 64 units below the spawn", &spawn );
		else
			printf( "  floor %.1f qu below the spawn, normal y %.3f\n",
				QU( spawn.v[1] - tr.end.v[1] ), WU( tr.nPlaneN[1] ) );
	}

	// --- 5. walking into a wall does not go through it -----------------
	// Every heading, 400 frames each, at a speed no Quake player reaches.
	// A hull trace that leaks does it on one heading out of dozens, which
	// is why this sweeps rather than samples.
	{
		unsigned nEscaped = 0, nStuck = 0, nHeadings = 64, nMoved = 0;
		double fBest = 0.0;
		for ( unsigned h = 0; h < nHeadings; h++ )
		{
			Gpu64_LevelVec p = spawn;
			const double a = 2.0 * M_PI * h / nHeadings;
			for ( unsigned f = 0; f < 400; f++ )
			{
				step( &p, a, 16.0, GPU64_CLIP_MODE_WALK );
				if ( !inBounds( &p ) ) { nEscaped++; fail( "walked out of the map", &p ); break; }
				if ( inSolid( &p ) )  { nStuck++;   fail( "walked into solid", &p );    break; }
			}
			const double d = sqrt( QU( p.v[0] - spawn.v[0] ) * QU( p.v[0] - spawn.v[0] ) +
					       QU( p.v[2] - spawn.v[2] ) * QU( p.v[2] - spawn.v[2] ) );
			if ( d > 32.0 ) nMoved++;
			if ( d > fBest ) fBest = d;
		}
		printf( "  %u headings x 400 frames at 16 qu: %u escaped, %u in solid,"
			" %u got somewhere, best %.0f qu\n",
			nHeadings, nEscaped, nStuck, nMoved, fBest );

		// The counters above are all zero if the trace blocks every move
		// -- which is the shape of "L= read 0/0 on a rung that was
		// killing the C64". A collision system that never lets anything
		// move passes every safety check there is.
		if ( nMoved * 2 < nHeadings )
			fail( "over half the headings went nowhere", 0 );
		if ( fBest < 200.0 )
			fail( "no heading got 200 qu from the spawn", 0 );
	}

	// --- 6. a wandering walk, deterministic ----------------------------
	// The sweep above only ever goes straight. This turns, which is what
	// puts the move into inside corners -- the case the crease fallback in
	// flyMove() exists for and the one a naive trace jitters in.
	{
		Gpu64_LevelVec p = spawn;
		unsigned nGround = 0, nWall = 0, nStep = 0, nBad = 0;
		u32 seed = 0x1234567u;
		double a = 0.0, fPath = 0.0, fFar = 0.0;
		double fHi = QU( spawn.v[1] ), fLo = QU( spawn.v[1] );
		for ( unsigned f = 0; f < 20000; f++ )
		{
			seed = seed * 1103515245u + 12345u;
			a += ( (double)( ( seed >> 16 ) & 0xff ) - 127.5 ) * 0.01;

			const Gpu64_LevelVec was = p;
			const u8 fl = step( &p, a, 12.0, GPU64_CLIP_MODE_WALK );

			fPath += sqrt( QU( p.v[0] - was.v[0] ) * QU( p.v[0] - was.v[0] ) +
				       QU( p.v[2] - was.v[2] ) * QU( p.v[2] - was.v[2] ) );
			const double d = sqrt( QU( p.v[0] - spawn.v[0] ) * QU( p.v[0] - spawn.v[0] ) +
					       QU( p.v[2] - spawn.v[2] ) * QU( p.v[2] - spawn.v[2] ) );
			if ( d > fFar ) fFar = d;
			if ( QU( p.v[1] ) > fHi ) fHi = QU( p.v[1] );
			if ( QU( p.v[1] ) < fLo ) fLo = QU( p.v[1] );
			if ( fl & GPU64_CLIP_ONGROUND ) nGround++;
			if ( fl & GPU64_CLIP_WALL )     nWall++;
			if ( fl & GPU64_CLIP_STEPPED )  nStep++;

			if ( !inBounds( &p ) || inSolid( &p ) )
			{
				nBad++;
				fail( "wander left the world", &p );
				break;
			}
		}
		printf( "  wander 20000 frames: %u on ground, %u wall, %u stepped, %u bad\n",
			nGround, nWall, nStep, nBad );
		printf( "  wander path %.0f qu, furthest %.0f qu from the spawn,"
			" %.0f qu of vertical range\n", fPath, fFar, fHi - fLo );

		if ( nGround * 4 < 20000 )
			fail( "the walk was airborne most of the time", 0 );
		if ( nWall == 0 )
			fail( "20000 frames of wandering never touched a wall", 0 );
		// 20000 frames at 12 qu is 240000 qu of intent. Sliding along
		// walls costs most of that, but a tenth of it is the difference
		// between walking the level and vibrating in one corner of it.
		if ( fPath < 24000.0 )
			fail( "the wander covered under a tenth of its intent", 0 );
		if ( fFar < 300.0 )
			fail( "the wander never got 300 qu from the spawn", 0 );
		if ( fHi - fLo < 32.0 )
			fail( "the wander never changed height", 0 );
	}

	// --- 7. a move of zero does not move -------------------------------
	{
		Gpu64_LevelVec d = { { 0, 0, 0 } };
		Gpu64_LevelMove mv;
		gpu64_levelMove( &g_L, 0, 1, GPU64_CLIP_MODE_SLIDE, &spawn, &d, &mv );
		if ( mv.end.v[0] != spawn.v[0] || mv.end.v[1] != spawn.v[1] ||
		     mv.end.v[2] != spawn.v[2] )
			fail( "a zero move moved", &mv.end );
	}

	// --- 8. the eye mode is exactly the offset -------------------------
	// Same move from the same place, once in origin coordinates and once
	// in eye coordinates, must differ by view_ofs and nothing else.
	{
		Gpu64_LevelVec eye = spawn, d;
		eye.v[ 1 ] += gpu64_levelEyeOfs( &g_L );
		d.v[ 0 ] = 65536 / 8; d.v[ 1 ] = 0; d.v[ 2 ] = 0;

		Gpu64_LevelMove a, b;
		gpu64_levelMove( &g_L, 0, 1, GPU64_CLIP_MODE_SLIDE, &spawn, &d, &a );
		gpu64_levelMove( &g_L, 0, 1, GPU64_CLIP_MODE_SLIDE | GPU64_CLIP_MODE_EYE,
				 &eye, &d, &b );
		if ( b.end.v[0] != a.end.v[0] ||
		     b.end.v[2] != a.end.v[2] ||
		     b.end.v[1] != a.end.v[1] + gpu64_levelEyeOfs( &g_L ) )
			fail( "eye mode is not a pure offset", &b.end );
	}

	// --- 9. hull 2 exists and is a different tree ----------------------
	if ( g_World.nHead[ 1 ] == g_World.nHead[ 0 ] )
		fail( "hull 2 is the same tree as hull 1", 0 );

	// --- 10. a bad hull number is refused, not guessed ------------------
	{
		Gpu64_LevelVec d = { { 0, 0, 0 } };
		Gpu64_LevelMove mv;
		if ( gpu64_levelMove( &g_L, 0, 0, 0, &spawn, &d, &mv ) )
			fail( "hull 0 was accepted", 0 );
		if ( gpu64_levelMove( &g_L, 0, 3, 0, &spawn, &d, &mv ) )
			fail( "hull 3 was accepted", 0 );
		if ( gpu64_levelMove( &g_L, 9999, 1, 0, &spawn, &d, &mv ) )
			fail( "a model past the hull table was accepted", 0 );
	}

	// --- 11. a closed door blocks, the same door opened does not -------
	//
	// This is the assertion that matters for movers, and it is two-sided on
	// purpose. "The door blocks" alone is passed by a tracer that blocks
	// everything -- the L=0/0 trap from the milestone 6a ladder. So every
	// testable door is swept three times: without the mover at all, with it
	// where the level put it, and with it displaced by its own travel. The
	// first and third must agree; the second must stop short.
	{
		const s32 u = gpu64_levelQU( &g_L, 1 );	// one Quake unit
		unsigned nTested = 0, nBlocked = 0, nOpened = 0;

		for ( unsigned i = 0; i < g_L.nEnt; i++ )
		{
			Gpu64_LevelEnt e;
			if ( !gpu64_levelEnt( &g_L, i, &e ) )
				continue;
			if ( e.nKind != 11 || e.nModelIdx == 0 )
				continue;
			if ( e.ofsX == 0 && e.ofsY == 0 && e.ofsZ == 0 )
				continue;

			Gpu64_LevelHull h;
			if ( !gpu64_levelHull( &g_L, e.nModelIdx, &h ) )
				continue;

			// Walk through the panel the short way: a door's own
			// box is a thin slab and its thinnest axis is the one
			// the doorway faces.
			unsigned a = 0;
			for ( unsigned k = 1; k < 3; k++ )
				if ( h.maxs.v[ k ] - h.mins.v[ k ] <
				     h.maxs.v[ a ] - h.mins.v[ a ] )
					a = k;
			s32 thick = h.maxs.v[ a ] - h.mins.v[ a ];

			Gpu64_LevelVec c, start, d;
			for ( unsigned k = 0; k < 3; k++ )
				c.v[ k ] = h.mins.v[ k ] +
					   ( h.maxs.v[ k ] - h.mins.v[ k ] ) / 2;
			start = c;
			start.v[ a ] = c.v[ a ] - thick / 2 - 24 * u;
			d.v[ 0 ] = d.v[ 1 ] = d.v[ 2 ] = 0;
			d.v[ a ] = thick + 48 * u;

			// A straight sweep, no slide and no floor settle: the
			// question here is what stops the move, not what a
			// walking player would do about it.
			Gpu64_LevelMove mNone, mShut, mOpen;
			Gpu64_LevelMover mv;
			mv.nModel = e.nModelIdx;

			if ( !gpu64_levelMove( &g_L, 0, 1, 0, &start, &d, &mNone ) )
				continue;
			// Only doors whose doorway is actually clear in the
			// world hull can say anything: if the world itself
			// stops the sweep, the door's contribution is not
			// observable and the geometry is not a test.
			if ( mNone.nFlags & ( GPU64_CLIP_STARTSOLID |
					      GPU64_CLIP_ALLSOLID ) )
				continue;
			if ( mNone.end.v[ a ] - start.v[ a ] != d.v[ a ] )
				continue;		// the world stops it

			mv.ofs.v[ 0 ] = mv.ofs.v[ 1 ] = mv.ofs.v[ 2 ] = 0;
			if ( !gpu64_levelMoveEnts( &g_L, 0, 1, 0, &start, &d,
						   &mv, 1, &mShut ) )
			{
				fail( "a mover was refused", &start );
				continue;
			}

			mv.ofs.v[ 0 ] = e.ofsX;
			mv.ofs.v[ 1 ] = e.ofsY;
			mv.ofs.v[ 2 ] = e.ofsZ;
			if ( !gpu64_levelMoveEnts( &g_L, 0, 1, 0, &start, &d,
						   &mv, 1, &mOpen ) )
			{
				fail( "an opened mover was refused", &start );
				continue;
			}

			// How far the sweep actually got along its own axis.
			// NOT nFraction: that field is the fraction of the
			// *horizontal* displacement, and a door whose thin
			// axis is vertical reads 255 while standing perfectly
			// still (model 8 on E1M1, a 14-unit slab in z).
			s32 gNone = mNone.end.v[ a ] - start.v[ a ];
			s32 gShut = mShut.end.v[ a ] - start.v[ a ];
			s32 gOpen = mOpen.end.v[ a ] - start.v[ a ];

			nTested++;
			if ( gShut < gNone - 2 * u )
				nBlocked++;
			else
				printf( "  note  door model %u axis %u thick %d qu: "
					"shut %d of %d qu, flags %02x/%02x\n",
					e.nModelIdx, a, thick / u, gShut / u,
					gNone / u, mShut.nFlags, mNone.nFlags );
			if ( gOpen == gNone )
				nOpened++;
			else
				printf( "  note  door model %u opened to %d of %d qu\n",
					e.nModelIdx, gOpen / u, gNone / u );
		}

		printf( "  doors: %u testable, %u blocked closed, %u clear open\n",
			nTested, nBlocked, nOpened );
		if ( nTested == 0 )
			fail( "no door was testable -- the geometry test is dead", 0 );
		if ( nBlocked != nTested )
			fail( "a closed door did not block", 0 );
		if ( nOpened != nTested )
			fail( "an opened door still blocked", 0 );
	}

	// --- 12. the mover list's own edges ---------------------------------
	{
		Gpu64_LevelVec d = { { 0, 0, 0 } };
		d.v[ 0 ] = 8 * gpu64_levelQU( &g_L, 1 );

		Gpu64_LevelMove a, b;
		gpu64_levelMove( &g_L, 0, 1, GPU64_CLIP_MODE_SLIDE, &spawn, &d, &a );

		// An empty slot is skipped, not traced as the world.
		Gpu64_LevelMover mv;
		mv.nModel = 0;
		mv.ofs.v[ 0 ] = mv.ofs.v[ 1 ] = mv.ofs.v[ 2 ] = 0;
		if ( !gpu64_levelMoveEnts( &g_L, 0, 1, GPU64_CLIP_MODE_SLIDE,
					   &spawn, &d, &mv, 1, &b ) )
			fail( "an empty mover slot was refused", 0 );
		else if ( b.end.v[0] != a.end.v[0] || b.end.v[1] != a.end.v[1] ||
			  b.end.v[2] != a.end.v[2] )
			fail( "an empty mover slot changed the move", &b.end );

		// A model the level does not have is refused, never ignored: a
		// mover that quietly stops blocking is how a player leaves the
		// map.
		mv.nModel = 9999;
		if ( gpu64_levelMoveEnts( &g_L, 0, 1, GPU64_CLIP_MODE_SLIDE,
					  &spawn, &d, &mv, 1, &b ) )
			fail( "a mover past the hull table was accepted", 0 );

		// And the list has a bound.
		Gpu64_LevelMover many[ GPU64_LEVEL_MAX_MOVERS ];
		for ( unsigned i = 0; i < GPU64_LEVEL_MAX_MOVERS; i++ )
			many[ i ] = mv;
		many[ 0 ].nModel = 0;
		if ( gpu64_levelMoveEnts( &g_L, 0, 1, GPU64_CLIP_MODE_SLIDE,
					  &spawn, &d, many,
					  GPU64_LEVEL_MAX_MOVERS + 1, &b ) )
			fail( "more movers than the bound were accepted", 0 );
	}

	printf( g_Fail ? "hulltest FAILED (%u)\n" : "hulltest ok (%u failures)\n", g_Fail );
	return g_Fail ? 1 : 0;
}
