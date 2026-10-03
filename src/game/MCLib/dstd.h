//---------------------------------------------------------------------------
//
// dstd.h - This file contains the standard definitions
//
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef DSTD_H
#define DSTD_H
//---------------------------------------------------------------------------
// Include files

//---------------------------------------------------------------------------
// Type Definitions
typedef unsigned char* MemoryPtr;
typedef unsigned char byte;

// PORT: `typedef unsigned size_t;` removed.
//
// MCLib redefined size_t as 32-bit. On the original x86 build that was
// identical to the CRT's definition, so the redefinition was invisible. On x64
// the CRT's size_t is 64-bit and this is a hard "typedef redefinition with
// different types" conflict, so the standard definition is used instead.
//
// CONSEQUENCE TO WATCH: any struct member declared size_t is now 8 bytes rather
// than 4. That is harmless for in-memory use, but it would change the layout of
// anything serialised field-by-field. The known serialisation path
// (MemoryStreamIO) is already handled -- its size_t overloads deliberately
// marshal through a 32-bit temporary to preserve the original wire width. If a
// #pragma pack struct turns out to carry a size_t, it will need the same
// treatment; that surfaces at M2 when real data loads.
#include <stddef.h>

#ifndef NULL
#define NULL	0
#endif

#ifndef TRUE
#define TRUE	1
#endif

#ifndef FALSE
#define FALSE	0
#endif

#define RADS_TO_DEGREES	57.2957795132f		// Used to convert results in Rad to Deg
#define DEGREES_TO_RADS 0.0174532925199f 	// Used to convert results in Deg to Rad
#define COS_SIN_45		0.70710678f		 	// Used to convert results in Deg to Rad
#define PI				3.1415926535897932384626433832795

typedef unsigned long DWORD;
typedef unsigned int UINT;
typedef unsigned char BYTE;
typedef void *PVOID;
//--------------------------------------------------------------------------
// Macro Definitions
#define NONE -1

//--------------------------------------------------------------------------
// Pragmas
#pragma warning(disable:4244)
#pragma warning(disable:4514)		// Unused Inline Functions
#pragma warning(disable:4800)
#pragma warning(disable:4244)

//---------------------------------------------------------------------------
#endif

//---------------------------------------------------------------------------
//
// Edit Log
//
//
//---------------------------------------------------------------------------
