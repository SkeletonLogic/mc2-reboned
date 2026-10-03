//***************************************************************************
//
//	SortList.cpp -- Sort List class
//
//	MechCommander II
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//--------------
// Include Files

#ifndef SORTLIST_H
#include "sortlist.h"
#endif

#ifndef ERR_H
#include "err.h"
#endif

#ifndef HEAP_H
#include "Heap.h" // PORT: match case-sensitive host path.
#endif

#include <stdlib.h>

#include <GameOS.HPP> // PORT: match case-sensitive host path.
//***************************************************************************
// Class SortList
//***************************************************************************

long SortList::init (long _numItems) {

	//-------------------------
	// Create the sort list...
	numItems = _numItems;
	list = (SortListNode*)systemHeap->Malloc(sizeof(SortListNode) * numItems);
	if (!list)
		Fatal(0, " Unable to init sortList ");
	return(list == NULL);
}

//---------------------------------------------------------------------------

void SortList::clear (bool setToMin) {
	long i;

	for (i = 0; i < numItems; i++)
		list[i].id = i;
	if (setToMin)
		for (i = 0; i < numItems; i++)
			list[i].value = float(-3.4E38);
	else
		for (i = 0; i < numItems; i++)
			list[i].value = float(3.4E38);
}

//---------------------------------------------------------------------------

// PORT: `cdecl` -> `__cdecl`. The unprefixed spelling was an old MSVC
// extension; clang parses it as an identifier, which is why this produced
// "expected ';' after top level declarator" and then a bogus "redefinition of
// 'cdecl'". On x64 there is only one calling convention, so this is a no-op
// that keeps the original intent legible.
int __cdecl descendingCompare (const void* arg1, const void* arg2 ) {

	float value1 = ((SortListNode*)arg1)->value;
	float value2 = ((SortListNode*)arg2)->value;
	if (value1 > value2)
		return(-1);
	else if (value1 < value2)
		return(1);
	else
		return(0);
}

//---------------------------------------------------------------------------

int __cdecl ascendingCompare (const void* arg1, const void* arg2 ) {	// PORT: see note above

	float value1 = ((SortListNode*)arg1)->value;
	float value2 = ((SortListNode*)arg2)->value;
	if (value1 > value2)
		return(1);
	else if (value1 < value2)
		return(-1);
	else
		return(0);
}

//---------------------------------------------------------------------------

void SortList::sort (bool descendingOrder) {

	//------------------------------------------------------------------
	// For now, just use ANSI C's built-in qsort (ugly, but functional).
	if (descendingOrder)
		qsort((void*)list, (size_t)numItems, sizeof(SortListNode), descendingCompare);
	else
		qsort((void*)list, (size_t)numItems, sizeof(SortListNode), ascendingCompare);
}

//---------------------------------------------------------------------------

void SortList::destroy (void) {

	systemHeap->Free(list);
	list = NULL;
}

//***************************************************************************

