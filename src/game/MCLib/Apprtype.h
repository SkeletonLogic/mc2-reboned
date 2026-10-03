//---------------------------------------------------------------------------
//
// ApprType.h -- File contains the Basic Game Appearance Type Declaration
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef APPRTYPE_H
#define APPRTYPE_H
//---------------------------------------------------------------------------
// Include Files
#ifndef DAPRTYPE_H
#include "Daprtype.h" // PORT: match case-sensitive host path.
#endif

#ifndef DAPPEAR_H
#include "Dappear.h" // PORT: match case-sensitive host path.
#endif

#ifndef DSTD_h
#include "dstd.h"
#endif

#ifndef HEAP_H
#include "Heap.h" // PORT: match case-sensitive host path.
#endif

#include <Stuff/StuffHeaders.hpp> // PORT: use host-independent include separators.

//---------------------------------------------------------------------------
// Macro definitions
#ifndef NO_ERR
#define NO_ERR					0
#endif

#define MAX_LODS				3
//---------------------------------------------------------------------------
// Class definitions
class AppearanceType
{
	//Data Members
	//-------------
	public:
	
		unsigned long 		numUsers;			//Number of users using this appearanceType.
		unsigned long		appearanceNum;		//What kind am I.
		AppearanceTypePtr	next;				//Pointer to next type in list.

		char 				*name;				//Appearance Base FileName.
	
		Stuff::Vector3D		typeUpperLeft;		//For Designer defined extents of objects
		Stuff::Vector3D		typeLowerRight;		//For Designer defined extents of objects

		float				boundsUpperLeftX;
		float				boundsUpperLeftY;
		
		float				boundsLowerRightX;
		float				boundsLowerRightY;
		
		bool				designerTypeBounds;	//So I know not to change them if the designer typed them in.

		// PORT (M8-B9): the TG_Shape::tglGeneration this type's shapes were
		// loaded under. getAppearance refuses to hand back an entry whose
		// generation is stale, because its shapes belong to a destroyed heap.
		unsigned long		tglGeneration;
		
	//Member Functions
	//-----------------
	public:
	
		void* operator new (size_t memSize);
		void operator delete (void* treePtr);

		void init (void)
		{
			numUsers = 0;
			next = NULL;
			appearanceNum = 0xffffffff;

			name = NULL;

			typeUpperLeft.Zero();
			typeLowerRight.Zero();
			designerTypeBounds = false;

			// PORT (M8-B9): 0 is never a live generation, so an unstamped type
			// always reads as stale rather than accidentally matching.
			tglGeneration = 0;
 		}

		AppearanceType (void)
		{
			init();
		}

		virtual void init (char *fileName);
		
		virtual void destroy (void);

		void reinit (void);

		bool typeBoundExists (void)
		{
			return true;		//Always exists now
		}

		bool getDesignerTypeBounds (void)
		{
			return designerTypeBounds;
		}

		unsigned long getAppearanceClass (void)
		{
			return(appearanceNum >> 24);
		}
		
		virtual ~AppearanceType (void)
		{
			destroy();
		}
};

//---------------------------------------------------------------------------
class AppearanceTypeList
{
	//Data Members
	//-------------
	protected:
	
		AppearanceTypePtr	head;
		AppearanceTypePtr	last;
		
	public:

		static UserHeapPtr	appearanceHeap;
		
	//Member Functions
	//----------------
	public:
	
		AppearanceTypeList (void)
		{
			head = last = NULL;
			appearanceHeap = NULL;
		}

		void init (unsigned long heapSize);

		AppearanceTypePtr getAppearance (unsigned long apprNum, char * apprFile);
		
		long removeAppearance (AppearanceTypePtr which);
		
		void destroy (void);
		
		~AppearanceTypeList (void)
		{
			destroy();
		}

		bool pointerCanBeDeleted (void *ptr);

		// PORT (M8-B9): how many types are cached right now. A tgl heap swap
		// orphans every one of them, so a swap that reports a non-zero count
		// is the moment their shapes go stale.
		long countTypes (void);
};

extern AppearanceTypeListPtr appearanceTypeList;
//---------------------------------------------------------------------------
#endif
