//---------------------------------------------------------------------------
//
// cellip.h - This file contains the class declarations for the ellipse element
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef CELLIP_H
#define CELLIP_H
//---------------------------------------------------------------------------
// Include files

#ifndef CELEMENT_H
#include "Celement.h" // PORT: match case-sensitive host path.
#endif

#ifndef VFX_H
#include "Vfx.h" // PORT: match case-sensitive host path.
#endif

#include "Utilities.h" // PORT: match case-sensitive host path.

#include <Stuff/Stuff.hpp> // PORT: use host-independent include separators.
//---------------------------------------------------------------------------
class EllipseElement : public Element
{
	public:

	gos_VERTEX location[5];
	GUI_RECT	clip;
		
	EllipseElement (void)
	{
		memset( location, 0, sizeof( location ) );
		memset( &clip, 0, sizeof( clip ) );
	}

	EllipseElement (Stuff::Vector2DOf<long> &cnt, Stuff::Vector2DOf<long> &ortho, long clr, long depth);

	virtual void draw (void);

	static void init(); // gotta call this one time before you can draw

	void setClip( const GUI_RECT& );

	static void removeTextureHandle (void);
	
	private:

		static	unsigned long s_textureHandle;
};

//---------------------------------------------------------------------------
#endif
