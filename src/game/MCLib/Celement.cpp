//---------------------------------------------------------------------------
//
// celement.cpp - This file contains the class declarations for the element pool
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include files

#ifndef CELEMENT_H
#include "Celement.h" // PORT: match case-sensitive host path.
#endif

#ifndef HEAP_H
#include "Heap.h" // PORT: match case-sensitive host path.
#endif

#include <GameOS.HPP> // PORT: match case-sensitive host path.

//---------------------------------------------------------------------------
// Static Data Members
//---------------------------------------------------------------------------
// Class Element
Element::Element (long _depth)
{
	depth = _depth;
	drawNormal = TRUE;
}
//---------------------------------------------------------------------------
// Class Element
Element::Element (float _depth)
{
	depth = _depth;
	drawNormal = TRUE;
}
	
//---------------------------------------------------------------------------
