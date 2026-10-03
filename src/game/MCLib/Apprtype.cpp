//---------------------------------------------------------------------------
//
// ApprType.cpp -- File contains the Basic Game Appearance Type code
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include Files
#ifndef APPRTYPE_H
#include "Apprtype.h" // PORT: match case-sensitive host path.
#endif

#ifndef GVACTOR_H
#include "Gvactor.h" // PORT: match case-sensitive host path.
#endif

#ifndef MECH3D_H
#include "mech3d.h"
#endif

#ifndef BDACTOR_H
#include "bdactor.h"
#endif

#ifndef GENACTOR_H
#include "genactor.h"
#endif

#include "Cident.h" // PORT: match case-sensitive host path.
#include "Inifile.h" // PORT: match case-sensitive host path.
#include "Paths.h" // PORT: match case-sensitive host path.

#include <GameOS.HPP> // PORT: match case-sensitive host path.

#include <stdio.h> // PORT (M8-B9): the stale-appearance report.

//---------------------------------------------------------------------------
// static Globals
UserHeapPtr AppearanceTypeList::appearanceHeap = NULL;
AppearanceTypeListPtr appearanceTypeList = NULL;

//---------------------------------------------------------------------------
// Class AppearanceType
void* AppearanceType::operator new (size_t memSize)
{
	void* result = NULL;
	if (AppearanceTypeList::appearanceHeap && AppearanceTypeList::appearanceHeap->heapReady())
	{
		result = AppearanceTypeList::appearanceHeap->Malloc(memSize);
	}
		
	return(result);
}

//---------------------------------------------------------------------------
void AppearanceType::operator delete (void* treePtr)
{
	long result;
	if (AppearanceTypeList::appearanceHeap && AppearanceTypeList::appearanceHeap->heapReady())
	{
		result = AppearanceTypeList::appearanceHeap->Free(treePtr);
	}
}

//---------------------------------------------------------------------------
void AppearanceType::init (char *fileName)
{
	name = (char *)AppearanceTypeList::appearanceHeap->Malloc(strlen(fileName)+1);
	strcpy(name,fileName);

	//Dig out the Type Bounds here for selections
	FullPathFileName iniName;
	iniName.init(tglPath,fileName,".ini");

	FitIniFile iniFile;
	long result = iniFile.open(iniName);
	if (result != NO_ERR)
		STOP(("Could not find appearance INI file %s",(char*)iniName));
		
	result = iniFile.seekBlock("3DBounds");
	if (result == NO_ERR)
	{
		designerTypeBounds = true;
		
		long tmpy;
		result = iniFile.readIdLong("UpperLeftX",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",(char*)iniName));
		else
			typeUpperLeft.x = tmpy;
			
		result = iniFile.readIdLong("UpperLeftY",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",(char*)iniName));
		else
			typeUpperLeft.y = tmpy;
  			
		result = iniFile.readIdLong("UpperLeftZ",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",(char*)iniName));
		else
			typeUpperLeft.z = tmpy;
			
  		result = iniFile.readIdLong("LowerRightX",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",(char*)iniName));
 		else
			typeLowerRight.x = tmpy;
    		
		result = iniFile.readIdLong("LowerRightY",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",(char*)iniName));
 		else
			typeLowerRight.y = tmpy;
			
		result = iniFile.readIdLong("LowerRightZ",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",(char*)iniName));
 		else
			typeLowerRight.z = tmpy;
    }
 
	iniFile.close();   
}	

//---------------------------------------------------------------------------
void AppearanceType::reinit (void)
{
	//Dig out the Type Bounds here for selections
	FullPathFileName iniName;
	iniName.init(tglPath,name,".ini");

	FitIniFile iniFile;
	long result = iniFile.open(iniName);
	if (result != NO_ERR)
		STOP(("Could not find appearance INI file %s",(char*)iniName));
		
	result = iniFile.seekBlock("3DBounds");
	if (result == NO_ERR)
	{
		designerTypeBounds = true;
		
		long tmpy;
		result = iniFile.readIdLong("UpperLeftX",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",(char*)iniName));
		else
			typeUpperLeft.x = tmpy;
			
		result = iniFile.readIdLong("UpperLeftY",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",(char*)iniName));
		else
			typeUpperLeft.y = tmpy;
  			
		result = iniFile.readIdLong("UpperLeftZ",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",(char*)iniName));
		else
			typeUpperLeft.z = tmpy;
			
  		result = iniFile.readIdLong("LowerRightX",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",(char*)iniName));
 		else
			typeLowerRight.x = tmpy;
    		
		result = iniFile.readIdLong("LowerRightY",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",(char*)iniName));
 		else
			typeLowerRight.y = tmpy;
			
		result = iniFile.readIdLong("LowerRightZ",tmpy);
		if (result != NO_ERR)
			STOP(("Cant find upperleftX type bounds in %s",(char*)iniName));
 		else
			typeLowerRight.z = tmpy;
    }
 
	iniFile.close();   
}	

//---------------------------------------------------------------------------
void AppearanceType::destroy (void)
{
	AppearanceTypeList::appearanceHeap->Free(name);
	name = NULL;
}

//---------------------------------------------------------------------------
// class AppearanceTypeList
void AppearanceTypeList::init (unsigned long heapSize)
{
	appearanceHeap = new UserHeap;
	gosASSERT(appearanceHeap != NULL);

	appearanceHeap->init(heapSize,"APPEAR");

	//No More sprite files.  Objects now have direct control over their appearances.
	
}

//---------------------------------------------------------------------------
AppearanceTypePtr AppearanceTypeList::getAppearance (unsigned long apprNum, char *appearFile)
{
	//----------------------------------------------------------------
	// The type of appearance is stored in the upper 8 bits of the
	// apprNum.  To get the correct packet we mask off the top 8 bits
	// and store the number.  To get the appearance type, we right shift
	// by 24.
	long appearanceClass = apprNum >> 24;
	AppearanceTypePtr appearanceType = NULL;
	
	//----------------------------------------------------
	// If these top bits are wrong, return NULL
	if (appearanceClass == 0)
		return(NULL);

	//----------------------------------------------------
	// If string passed in is NULL, return NULL
	if (!appearFile)
	{
		return NULL;
	}

	//-----------------------------------------------------------
	// Scan the list of appearances and see if we have this one.
	AppearanceTypePtr previous = NULL;
	appearanceType = head;
	while (appearanceType && stricmp(appearanceType->name,appearFile) != 0)
	{
		previous = appearanceType;
		appearanceType = appearanceType->next;
	}

	// PORT (M8-B9): a cached type holds TG_TypeMultiShape pointers into
	// TG_Shape::tglHeap. Mission::initTGLForLogistics() and the mission
	// loader BOTH destroy that heap and build another one, and neither
	// touches this cache -- so every shape a surviving entry points at is
	// freed memory that the next allocation reuses.
	//
	// MEASURED 2026-08-19, the crash that opened M8-B9: the entry returned
	// here had numUsers 1 -> 2 (a cache hit, not a fresh load), its
	// mechShape[0] pointed at a live GUI text array carrying the mission
	// briefing, and loading listOfTypeShapes out of it produced the
	// non-canonical address 0x72206F7470797243 -- the ASCII bytes
	// "Crypto r" out of "Steiner Crypto reports that ...". Windows reports
	// a non-canonical access as a read from 0xFFFFFFFFFFFFFFFF, which is why
	// that crash first looked like a sentinel being dereferenced.
	//
	// Treat a stale generation as a miss: unlink it so it can never be
	// handed out again, and load the shape fresh below.
	if (appearanceType && (appearanceType->tglGeneration != TG_Shape::tglGeneration))
	{
		fprintf(stderr,
			"[tgl-gen] STALE APPEARANCE TYPE '%s': loaded under tgl generation "
			"%lu, current is %lu -- its shapes were freed with that heap. "
			"Unlinking and reloading.\n",
			appearFile, appearanceType->tglGeneration, TG_Shape::tglGeneration);
		fflush(stderr);

		if (previous)
			previous->next = appearanceType->next;
		else
			head = appearanceType->next;

		if (appearanceType == last)
			last = previous;

		// DELIBERATELY NOT DELETED. Its destroy() walks the very shape
		// pointers that are already freed. The type itself lives in
		// appearanceHeap, which is released in one go, so dropping it here
		// leaks nothing beyond that heap's own lifetime.
		appearanceType = NULL;
	}

	if (appearanceType)
	{
		appearanceType->numUsers++;
	}
	else
	{
		//---------------------------------------------------------
		// This appearance is not on the list yet, so load/new it.	
		switch (appearanceClass)
		{
			//----------------------------------------
			case MECH_TYPE:
			{
				appearanceType = new Mech3DAppearanceType;
				gosASSERT(appearanceType != NULL);

				appearanceType->appearanceNum = apprNum;
				appearanceType->init(appearFile);

				//----------------------------------------
				// We have a new one, add it to the list.				
				appearanceType->numUsers = 1;
				appearanceType->next = NULL;

				if (head == NULL)
				{
					head = appearanceType;
					last = head;
				}
				else
				{
					last->next = appearanceType;
					last = appearanceType;
				}
			}
			break;

			//----------------------------------------
			case GV_TYPE:
			{
				appearanceType = new GVAppearanceType;
				gosASSERT(appearanceType != NULL);

				appearanceType->appearanceNum = apprNum;
				appearanceType->init(appearFile);

				//----------------------------------------
				// We have a new one, add it to the list.				
				appearanceType->numUsers = 1;
				appearanceType->next = NULL;

				if (head == NULL)
				{
					head = appearanceType;
					last = head;
				}
				else
				{
					last->next = appearanceType;
					last = appearanceType;
				}
			}
			break;
			
			//----------------------------------------
			case TREED_TYPE:
			{
				appearanceType = new TreeAppearanceType;
				gosASSERT(appearanceType != NULL);

				appearanceType->appearanceNum = apprNum;
				appearanceType->init(appearFile);

				//----------------------------------------
				// We have a new one, add it to the list.				
				appearanceType->numUsers = 1;
				appearanceType->next = NULL;

				if (head == NULL)
				{
					head = appearanceType;
					last = head;
				}
				else
				{
					last->next = appearanceType;
					last = appearanceType;
				}
			}
			break;

			//----------------------------------------
			case GENERIC_APPR_TYPE:
			{
				appearanceType = new GenericAppearanceType;
				gosASSERT(appearanceType != NULL);

				appearanceType->appearanceNum = apprNum;
				appearanceType->init(appearFile);

				//----------------------------------------
				// We have a new one, add it to the list.				
				appearanceType->numUsers = 1;
				appearanceType->next = NULL;

				if (head == NULL)
				{
					head = appearanceType;
					last = head;
				}
				else
				{
					last->next = appearanceType;
					last = appearanceType;
				}
			}
			break;

			//----------------------------------------
			case BLDG_TYPE:
			{
				appearanceType = new BldgAppearanceType;
				gosASSERT(appearanceType != NULL);

				appearanceType->appearanceNum = apprNum;
				appearanceType->init(appearFile);

				//----------------------------------------
				// We have a new one, add it to the list.				
				appearanceType->numUsers = 1;
				appearanceType->next = NULL;

				if (head == NULL)
				{
					head = appearanceType;
					last = head;
				}
				else
				{
					last->next = appearanceType;
					last = appearanceType;
				}
			}
			break;

			default:
				return(NULL);
		}

		// PORT (M8-B9): stamp the heap generation these shapes were just
		// loaded under, so a later swap of that heap makes this entry read
		// as stale instead of handing back freed geometry.
		if (appearanceType)
			appearanceType->tglGeneration = TG_Shape::tglGeneration;
	}
	
	return appearanceType;
}
		
//---------------------------------------------------------------------------
long AppearanceTypeList::removeAppearance (AppearanceTypePtr which)
{
	AppearanceTypePtr appearanceType = head;
	AppearanceTypePtr previous = NULL;
	
	while (appearanceType && (appearanceType != which))
	{
		previous = appearanceType;
		appearanceType = appearanceType->next;
	}

	if (appearanceType)
		appearanceType->numUsers--;
	else
		return(-1);
	
	//----------------------------------------------------------
	// NEVER remove the types anymore.  Save cache time!
//#if 0
	if (appearanceType && (appearanceType->numUsers == 0))
	{
		//------------------------------------------------------
		// Check if there is no previous appearanceType in list
		if (previous == NULL)
		{
			//----------------------------------------------------
			// if there was no previous, head is the next in list
			head = appearanceType->next;
		}
		else
		{
			previous->next = appearanceType->next;
		}
		
		//-------------------------------------------------------------
		// Make sure that we don't gratuitously free the last pointer
		if (appearanceType == last)
			last = previous;

		// PORT (M8-B9): the SAME hazard as getAppearance() and destroy(), at a
		// third site. This delete runs the virtual ~AppearanceType, hence
		// destroy(), hence the walk over shape pointers that a tgl heap swap
		// already freed. A stale type whose last user is released after that
		// swap faults here exactly as it did at shutdown.
		//
		// Found 2026-08-21 while fixing the shutdown crash, by asking where
		// ELSE an appearance type gets destroyed. It has no observed symptom
		// of its own -- hypothesis, unverified, that it has ever fired -- but
		// it is the same measured mechanism reached by a different path, so it
		// gets the same treatment: unlinked above, abandoned here.
		if (appearanceType->tglGeneration != TG_Shape::tglGeneration)
		{
			fprintf(stderr,
				"[tgl-gen] AppearanceTypeList::removeAppearance is ABANDONING a type "
				"stamped tgl generation %lu (current is %lu) instead of deleting it -- "
				"its shapes were freed with that heap.\n",
				appearanceType->tglGeneration, TG_Shape::tglGeneration);
			fflush(stderr);

			appearanceType = NULL;
			return NO_ERR;
		}

		delete appearanceType;
		appearanceType = NULL;
	}
//#endif
	
	return NO_ERR;
}

//---------------------------------------------------------------------------
void AppearanceTypeList::destroy (void)
{
	//---------------------------------------------------------------------
	// Run through the list and force a destroy call for each list element.
	// This will free any RAM used by each individual appearance.
	// DO NOT really need to do this.  Just toss the heap!
	
	AppearanceTypePtr currentAppr = head;
	while (currentAppr)
	{
		AppearanceTypePtr nextPtr = currentAppr->next;

		// PORT (M8-B9): a type stamped with a dead tgl generation must NOT be
		// destroyed here, for exactly the reason getAppearance() above refuses
		// to hand one back: every shape it points at was freed with that heap.
		// Its destroy() would both READ that freed memory and, through
		// TG_TypeMultiShape::operator delete, hand the stale pointer to
		// whatever tglHeap is CURRENT -- two separate corruptions.
		//
		// MEASURED 2026-08-21 from mc2-run\crash-2696-1.dmp, the shutdown
		// crash (exit 0xC0000005 at TerminateGameEngine) that finally caught
		// this. Identity established, not inferred:
		//   * the faulting object was this list's ONLY entry, a MECH_TYPE
		//     stamped tglGeneration 9, numUsers 1, next NULL;
		//   * the single [tgl-gen] line of that session names generation 9 as
		//     the one initTGLForLogistics destroyed with 1 type still cached;
		//   * its mechShape[0] held 0x22F5CE60AA0, and those bytes had become
		//     the ASCII string
		//     'defaulted from [Texture "assets\graphics\arialnarrow9.d3f"]',
		//     so the load of listOfTypeShapes at +8 produced
		//     0x5B206D6F72662064 ("d from [") -- non-canonical, which Windows
		//     reports as a read from 0xFFFFFFFFFFFFFFFF rather than as a
		//     sentinel.
		// The competing explanation -- an unrelated teardown-order or
		// double-free bug in this path -- is eliminated: it would have faulted
		// on a type stamped with the LIVE generation, and no such entry exists.
		//
		// Everything destroy() would have freed that is NOT tgl memory (name,
		// nodeData, nodeId) lives in appearanceHeap, deleted in one go just
		// below, so skipping the call leaks nothing beyond that heap's own
		// lifetime.
		//
		// THIS REPORT CAN COME BACK NEGATIVE: a shutdown with no stale entry
		// prints nothing at all.
		if (currentAppr->tglGeneration != TG_Shape::tglGeneration)
		{
			fprintf(stderr,
				"[tgl-gen] AppearanceTypeList::destroy is SKIPPING a type stamped "
				"tgl generation %lu (current is %lu) -- its shapes were freed with "
				"that heap, so destroying it would fault. Abandoned, not deleted.\n",
				currentAppr->tglGeneration, TG_Shape::tglGeneration);
			fflush(stderr);

			currentAppr = nextPtr;
			continue;
		}

		currentAppr->destroy();
		currentAppr = nextPtr;
	}
	

	head = last = NULL;
	
	delete appearanceHeap;
	appearanceHeap = NULL;
	
 	if (GVAppearanceType::SensorCircleShape)
	{
		delete GVAppearanceType::SensorCircleShape;
		GVAppearanceType::SensorCircleShape = NULL;
	}
	
	if (GVAppearanceType::SensorTriangleShape)
	{
		delete GVAppearanceType::SensorTriangleShape;
		GVAppearanceType::SensorTriangleShape = NULL;
	}
	
	if (Mech3DAppearanceType::SensorSquareShape)
	{
		delete Mech3DAppearanceType::SensorSquareShape;
		Mech3DAppearanceType::SensorSquareShape = NULL;
	}
}

//---------------------------------------------------------------------------
bool AppearanceTypeList::pointerCanBeDeleted (void *ptr)
{
	if (appearanceHeap && appearanceHeap->pointerOnHeap(ptr))
		return true;

	return false;
}

//---------------------------------------------------------------------------
// PORT (M8-B9): the tgl heap swap frees every shape a cached type points at
// and nothing here is told about it. This makes the swap visible at the
// moment it happens, rather than at the crash three screens later.
long AppearanceTypeList::countTypes (void)
{
	long count = 0;

	AppearanceTypePtr cur = head;
	while (cur)
	{
		count++;
		cur = cur->next;
	}

	return count;
}

//---------------------------------------------------------------------------
