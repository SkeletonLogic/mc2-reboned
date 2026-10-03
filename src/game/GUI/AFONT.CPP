//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include "AFONT.H" // PORT: match case-sensitive host path.
#include "mclib.h"
#include "../resource.h" // PORT: browser-safe font fallbacks use named game resource IDs.
#include <mbstring.h>
#include <vector> // PORT (M8-B10 tail): the wrap scratch is sized to the input.

aFont::aFont()
{
	gosFont = 0;
	fontName[0] = 0;
	resID = 0;
	size = 1;
}

long aFont::init( const char* newFontName )
{
	destroy();
	gosFont = 0;

	strcpy( fontName, newFontName );
	char path[256];

	char* pStr = strstr( fontName, "," );
	if ( pStr )
	{
		size = atoi( pStr + 1 );
	}


	strcpy( path, "assets\\graphics\\" );
	strcat( path, fontName );
	_strlwr( path );
	gosFont = gos_LoadFont( path );

	if ( gosFont )
		return 0;

	return FONT_NOT_LOADED;

}

long aFont::init( long resourceID )
{
	destroy();
	gosFont = 0;
	gosFont = loadFont( resourceID, size );
	resID = resourceID;

	if ( gosFont )
		return 0;

	return FONT_NOT_LOADED;
}


aFont::~aFont()
{
	destroy();
}



unsigned long aFont::height( ) const
{
	unsigned long width, height;
	gos_TextSetAttributes (gosFont, 0, size, false, true, false, false);
	gos_TextStringLength(&width,&height,"ABCDE");
	return height;
}

void	aFont::getSize( unsigned long& width, unsigned long& height, const char* pText )
{
	gos_TextSetAttributes (gosFont, 0, size, false, true, false, false);
	gos_TextStringLength(&width,&height,pText);

}


unsigned long aFont::height( const char* st, int areaWidth ) const
{
	// PORT (M8-B24): THIS USED TO RE-IMPLEMENT WORD WRAP, AND COULD NOT WORK.
	//
	// The old body measured the string, then decided whether to run its own
	// wrap counter with `if ( width > areaWidth - 1 )`, returning
	// lineHeightOfA * lineCount with lineCount starting at 1.
	//
	// That condition assumes the measurement returns the UNWRAPPED width, which
	// is what retail's gos_TextStringLength did. In this port StringLength runs
	// the SAME Layout that draws the text, so the width it returns is the widest
	// WRAPPED line and is bounded by the region. The condition is therefore false
	// for anything but a freak exact fit, lineCount stayed 1, and a panel of any
	// height was sized as a single line.
	//
	// That is the owner's clipped briefing. "Liberate Gulag" is 879 characters
	// over 14 lines and every line is narrow, so nothing comes near the limit --
	// which is also why it is not the longest briefing that clips. It ranks about
	// 20th of 48 by length and roughly mid-pack by line count.
	//
	// Measured in tests/renderer_font_test.cpp with a synthetic font, region 40:
	// a four-line string returns width 10 (test does not fire) and height 24
	// (correct). The answer was already in hand and was being thrown away.
	//
	// So ask, and return it. This does not re-implement wrapping -- it DELETES a
	// second implementation of it, which is what R1-3 asked for: "driving both
	// over the retail string set and diffing line counts". One implementation now
	// sizes the panel and draws the text, so they cannot disagree.
	//
	// PORT (M8-B26): AND IT STILL MEASURED WITH WORD WRAP TURNED OFF.
	//
	// The rewrite above asks Layout for the height, which is right -- but it
	// asked with argument 4 of gos_TextSetAttributes set FALSE, and argument 4
	// is WordWrap. aFont::render sets it TRUE. Layout only wraps when that flag
	// is on, so the panel was sized in EXPLICIT NEWLINES and then drawn WRAPPED,
	// and every line the wrap added fell past the region bottom, where
	// ClipTextQuad drops it. Same mistake as M8-B22 and M8-B24: measured with
	// one set of metrics, drawn with another.
	//
	// MEASURED, from the owner's two screenshots of 2026-08-26:
	//
	//   55012 Killdeer  15 newline-separated segments  15 lines drawn, then cut
	//   55011 Hijack    24 newline-separated segments  24 lines drawn, then cut
	//
	// The cut lands on the segment count, exactly, for two different strings.
	// THE COMPETING EXPLANATION -- that the list box or its scroll extent is
	// short -- predicts both cut at the SAME line, the box height (about 21
	// lines at 1080p). They cut at 15 and 24. Ruled out.
	unsigned long width = 0, height = 0;
	gos_TextSetAttributes( gosFont, 0, size, true, true, false, false );
	gos_TextSetRegion( 0, 0, areaWidth, Environment.screenHeight );
	gos_TextStringLength( &width, &height, st );
	return height;
}

unsigned long aFont::width( const char* st ) const
{
	unsigned long width, height;
	gos_TextSetAttributes (gosFont, 0, size, false, true, false, false);
	gos_TextStringLength(&width,&height,st);
	return width;
}

long aFont::load( const char* fontName )
{
	destroy();

	return init( fontName );
}

void aFont::destroy()
{
	if ( gosFont )
		gos_DeleteFont( gosFont );

	gosFont = 0;
	resID = 0;
	fontName[0] = 0;

}

void aFont::render( const char* text, int xPos, int yPos, int areaWidth, int areaHeight, 
				   unsigned long color, bool bBold, int alignment )
{
	gos_TextSetAttributes( gosFont, color, size, true, true, bBold, false, alignment );
	
	if ( areaWidth < 1 )
	{
		if ( alignment == 1 )
		{
			unsigned long width, height;
			gos_TextStringLength( &width, &height, text );
			xPos -= width;
			areaWidth = width + 1;
		}
		else
			areaWidth = Environment.screenWidth;
	}

	if ( areaHeight < 1 )
	{
		if ( alignment == 3  ) // bottom
		{
			unsigned long width, height;
			gos_TextStringLength( &width, &height, text );
			yPos -= height;
			areaHeight = height + 1;
		}
		
		else
			areaHeight = Environment.screenHeight;
	}

	gos_TextSetRegion( xPos, yPos, xPos + areaWidth, yPos + areaHeight );
	
	gos_TextSetPosition( xPos, yPos );
	gos_TextDraw( text );	

}

HGOSFONT3D aFont::loadFont( long resourceID, long& size )
{
	size = 1;
	char buffer[256];
	cLoadString( resourceID, buffer, 255 );
	#if defined(__EMSCRIPTEN__)
		// PORT: MC2RES.DLL and its extracted retail text cannot ship in the web
		// payload. These two filenames are already named by game source and are
		// the only resource-backed fonts required by direct mission startup.
		if (!buffer[0] && resourceID == IDS_OBJECTIVE_MARKER_FONT)
			strcpy(buffer, "arialBlack9.d3f");
		else if (!buffer[0] && resourceID == IDS_FLOAT_HELP_FONT)
			strcpy(buffer, "arial8.d3f");
	#endif

	char* pStr = strstr( buffer, "," );
	if ( pStr )
	{
		size = -atoi( pStr + 1 );
		*pStr = NULL;
	}

	char path[256];
	strcpy( path, "assets\\graphics\\" );
	strcat( path, buffer );
	_strlwr( path );
	HGOSFONT3D retFont = gos_LoadFont( path );

	return retFont;
}

aFont::aFont( const aFont& src )
{
	copyData(src);
}

aFont& aFont::operator=( const aFont& src )
{
	copyData(src);
	return *this;
}

void aFont::copyData( const aFont& src )
{
	if ( &src != this )
	{
		if (src.resID )
		{
			init( src.resID );
		}
		else if ( src.fontName[0] != 0 )
		{
			init( src.fontName );
		}

		size = src.size;
	}
}
