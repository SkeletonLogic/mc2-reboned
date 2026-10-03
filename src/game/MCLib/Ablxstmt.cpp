//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//***************************************************************************
//
//								ABLEXEC.CPP
//
//***************************************************************************

#include <stdio.h>
#include <string.h>

#ifndef ABLGEN_H
#include "Ablgen.h" // PORT: match case-sensitive host path.
#endif

#ifndef ABLERR_H
#include "Ablerr.h" // PORT: match case-sensitive host path.
#endif

#ifndef ABLSCAN_H
#include "Ablscan.h" // PORT: match case-sensitive host path.
#endif

#ifndef ABLSYMT_H
#include "Ablsymt.h" // PORT: match case-sensitive host path.
#endif

#ifndef ABLPARSE_H
#include "Ablparse.h" // PORT: match case-sensitive host path.
#endif

#ifndef ABLEXEC_H
#include "Ablexec.h" // PORT: match case-sensitive host path.
#endif

#ifndef ABLDBUG_H
#include "Abldbug.h" // PORT: match case-sensitive host path.
#endif

//***************************************************************************

//----------
// EXTERNALS

extern long				level;
extern long				CallStackLevel;
extern long				execLineNumber;
extern long				execStatementCount;
extern char*			codeSegmentPtr;
extern char*			statementStartPtr;
extern TokenCodeType	codeToken;
extern long				NumExecutions;

extern StackItem*		stack;
extern StackItemPtr		tos;
extern StackItemPtr		stackFrameBasePtr;
extern SymTableNodePtr	CurRoutineIdPtr;

extern TypePtr			IntegerTypePtr;
extern TypePtr			CharTypePtr;
extern TypePtr			RealTypePtr;
extern TypePtr			BooleanTypePtr;

extern bool				ExitWithReturn;
extern bool				ExitFromTacOrder;
extern bool				AutoReturnFromOrders;

extern long				MaxLoopIterations;

extern DebuggerPtr		debugger;
extern ABLModulePtr		CurModule;
extern ABLModulePtr		CurFSM;
extern SymTableNodePtr	CurModuleIdPtr;
extern long				CurModuleHandle;
extern bool				CallModuleInit;
extern StackItemPtr		StaticDataPtr;
unsigned long*	OrderCompletionFlags = NULL;
extern ModuleEntryPtr	ModuleRegistry;
extern ABLModulePtr*	ModuleInstanceRegistry;
extern ABLModulePtr		CurModule;
extern ABLModulePtr		CurLibrary;
extern long				ProfileLogFunctionTimeLimit;
extern ABLFile*			ProfileLog;
extern bool				NewStateSet;

long	dummyCount = 0;

void execOrderReturn (long returnVal);
void ABL_AddToProfileLog (char* profileString);
void transState (SymTableNodePtr newState);

//***************************************************************************
//
//***************************************************************************

// PORT: M6-5 ABL order census. The downstream census proved the AI selects a
// target and then never issues an attack order, so the question moved up here:
// which order routines does the brain actually reach, which are skipped as
// already-complete, and which return 0 -- because an order returning 0 halts
// the rest of the orders block for the frame. Gated on MC2_COMBAT_CENSUS so the
// strict fixed-60 regression emits exactly the bytes it always has.
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace {
struct AblOrderCensus
{
    static const int MAX_NAMES = 48;
    char names[MAX_NAMES][32];
    long calls[MAX_NAMES];
    long skipped[MAX_NAMES];
    long returnedZero[MAX_NAMES];
    int  numNames;

    AblOrderCensus() : numNames(0) {}

    int slot(const char* name)
    {
        if (!name) name = "(unnamed)";
        for (int i = 0; i < numNames; ++i)
            if (std::strcmp(names[i], name) == 0) return i;
        if (numNames == MAX_NAMES) return -1;
        int i = numNames++;
        std::strncpy(names[i], name, sizeof(names[i]) - 1);
        names[i][sizeof(names[i]) - 1] = 0;
        calls[i] = skipped[i] = returnedZero[i] = 0;
        return i;
    }

    ~AblOrderCensus()
    {
        const char* want = std::getenv("MC2_COMBAT_CENSUS");
        if (!want || !*want || std::strcmp(want, "0") == 0) return;
        for (int i = 0; i < numNames; ++i)
            std::printf("[abl-census] order %-24s calls=%ld skipped=%ld returned0=%ld\n",
                        names[i], calls[i], skipped[i], returnedZero[i]);
    }
};
AblOrderCensus g_ablOrderCensus;
} // namespace


void execStatement (void) {

	if (codeToken == TKN_STATEMENT_MARKER) {
		execLineNumber = getCodeStatementMarker();
		execStatementCount++;

		statementStartPtr = codeSegmentPtr;

		if (debugger)
			debugger->traceStatementExecution();

		getCodeToken();
	}

	switch (codeToken) {
		case TKN_IDENTIFIER: {
			SymTableNodePtr idPtr = getCodeSymTableNodePtr();
			ABL_Assert(idPtr != NULL, 0, " oops ");
			if (idPtr->defn.key == DFN_FUNCTION) {
				bool skipOrder = false;
				unsigned char orderDWord = 0;
				unsigned char orderBitMask = 0;
				if ((idPtr->defn.info.routine.flags & ROUTINE_FLAG_ORDER) && CurModule->getOrderCallFlags()) {
					orderDWord = getCodeByte();
					orderBitMask = getCodeByte();
					skipOrder = !CurModule->isLibrary() && CurModule->getOrderCallFlag(orderDWord, orderBitMask);
				}
				int censusSlot = -1;   // PORT: M6-5 ABL order census
				if (idPtr->defn.info.routine.flags & ROUTINE_FLAG_ORDER) {
					censusSlot = g_ablOrderCensus.slot(idPtr->name);
					if (censusSlot >= 0) {
						++g_ablOrderCensus.calls[censusSlot];
						if (skipOrder) ++g_ablOrderCensus.skipped[censusSlot];
					}
				}
				TypePtr returnType = execRoutineCall(idPtr, skipOrder);
				if (idPtr->defn.info.routine.flags & ROUTINE_FLAG_ORDER) {
					if (AutoReturnFromOrders) {
						//-----------------------------------------------------------------
						// We called an Order function, and we're in an Orders/State block,
						// so do we continue the flow of orders or stop here?
						long returnVal = tos->integer;
						if ((censusSlot >= 0) && (returnVal == 0))   // PORT: M6-5 ABL order census
							++g_ablOrderCensus.returnedZero[censusSlot];
						pop();
						if (returnVal == 0)
							execOrderReturn(returnVal);
						else if (CurModule->getOrderCallFlags()) {
							CurModule->setOrderCallFlag(orderDWord, orderBitMask);
						}
					}
					}
				else if (returnType) {
						//------------------------------------------
						// In case this routine returns a value, pop
						// the return value off the stack...
						pop();
				}
				}
			else
				execAssignmentStatement(idPtr);
			}
			break;
		case TKN_CODE: {
			bool wasAutoReturnFromOrders = AutoReturnFromOrders;
			AutoReturnFromOrders = ((CurRoutineIdPtr->defn.info.routine.flags & (ROUTINE_FLAG_ORDER + ROUTINE_FLAG_STATE)) != 0);
			getCodeToken();
			TokenCodeType endToken = TKN_END_FUNCTION;
			if (CurRoutineIdPtr->defn.info.routine.flags & ROUTINE_FLAG_ORDER)
				endToken = TKN_END_ORDER;
			else if (CurRoutineIdPtr->defn.info.routine.flags & ROUTINE_FLAG_STATE)
				endToken = TKN_END_STATE;
			TokenCodeType endTokenFinal = TKN_END_MODULE;
			if (CurLibrary)
				endTokenFinal = TKN_END_LIBRARY;
			else if (CurRoutineIdPtr->defn.info.routine.flags & ROUTINE_FLAG_FSM)
				endTokenFinal = TKN_END_FSM;
			while ((codeToken != endToken) && (codeToken != endTokenFinal) && !NewStateSet)
				execStatement();
			if (NewStateSet)
				return;
			getCodeToken();
			AutoReturnFromOrders = wasAutoReturnFromOrders;
			}
			break;
		case TKN_FOR:
			execForStatement();
			break;
		case TKN_IF:
			execIfStatement();
			break;
		case TKN_REPEAT:
			execRepeatStatement();
			break;
		case TKN_WHILE:
			execWhileStatement();
			break;
		case TKN_SWITCH:
			execSwitchStatement();
			break;
		case TKN_TRANS:
			execTransStatement();
			break;
		case TKN_TRANS_BACK:
			execTransBackStatement();
			break;
		case TKN_SEMICOLON:
		case TKN_ELSE:
		case TKN_UNTIL:
			break;
		default:
			runtimeError(ABL_ERR_RUNTIME_UNIMPLEMENTED_FEATURE);
	}

	while (codeToken == TKN_SEMICOLON)
		getCodeToken();
}

//***************************************************************************

// PORT: B1-1. Declared rather than included: gos_memory_internal.h needs
// gameos.hpp for HGOSHEAP, and this file has no business seeing either.
size_t gos_memory_BlockSize(const void* p);
bool gos_memory_BlockExtent(const void* p, const void** base, size_t* userBytes);

//---------------------------------------------------------------------------
// PORT: B1-1. Env-gated, off by default, and it prints ONLY when a copy is
// genuinely out of bounds -- silence here is a real negative.
//
// gos_memory_BlockSize returns 0 for anything that is not the start of a live
// gos_Malloc block, which covers ABL values living inside a larger record or
// on the stack. Those are counted as unknown, never as violations.
//---------------------------------------------------------------------------
// PORT: B1-1. Clamp the array/record copy to what the SOURCE actually holds.
//
// Off by default and deliberately so: this changes what ABL copies, which
// changes AI behaviour and would move the strict bar. Arm it to let a
// page-guarded run get past the over-read and hunt for something else.
bool MC2_AblCopyClampEnabled(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		const char* want = getenv("MC2_ABL_COPY_CLAMP");
		enabled = (want && *want && strcmp(want, "0") != 0) ? 1 : 0;
		if (enabled)
			fprintf(stderr, "[abl-copy] CLAMP ARMED: array assignments copy the source's"
				" declared size, not the target's. AI behaviour differs from the bar.\n");
	}
	return enabled != 0;
}

void MC2_AblCopyCensus(const void* dest, const void* src, long size, long srcTypeSize)
{
	static int enabled = -1;
	if (enabled < 0) {
		const char* want = getenv("MC2_ABL_COPY_CENSUS");
		enabled = (want && *want && strcmp(want, "0") != 0) ? 1 : 0;
		if (enabled)
			fprintf(stderr, "[abl-copy] census armed: reporting array assignments "
				"whose length exceeds a known gos block\n");
	}
	if (!enabled || size <= 0)
		return;

	// Bound each end by the block that CONTAINS it. A start-only lookup was
	// silent through the very run that faulted here, because an ABL array value
	// sits inside a larger record allocation.
	const void* destBase = 0; size_t destBytes = 0;
	const void* srcBase  = 0; size_t srcBytes  = 0;
	const bool destKnown = gos_memory_BlockExtent(dest, &destBase, &destBytes);
	const bool srcKnown  = gos_memory_BlockExtent(src,  &srcBase,  &srcBytes);

	const size_t destRoom = destKnown ? destBytes - (size_t)((const char*)dest - (const char*)destBase) : 0;
	const size_t srcRoom  = srcKnown  ? srcBytes  - (size_t)((const char*)src  - (const char*)srcBase)  : 0;
	const bool destBad = destKnown && (size_t)size > destRoom;
	const bool srcBad  = srcKnown  && (size_t)size > srcRoom;
	if (!destBad && !srcBad)
		return;

	// Count every one by category; the detailed reports are capped but the
	// tally is not, because 'does the WRITE end ever overrun' is the question
	// that decides whether this is the corruptor.
	static long readOnly = 0, writeOnly = 0, bothEnds = 0, reported = 0;
	if (destBad && srcBad) bothEnds++; else if (destBad) writeOnly++; else readOnly++;
	if (((readOnly + writeOnly + bothEnds) % 100) == 0)
		fprintf(stderr, "[abl-copy] tally: read-overrun=%ld WRITE-overrun=%ld both=%ld\n",
			readOnly, writeOnly, bothEnds);
	if (++reported > 40)
		return;
	fprintf(stderr,
		"[abl-copy] OUT OF BOUNDS %s: copy length=%ld, source TYPE size=%ld\n"
		"[abl-copy]   dest=%p room=%zu (block %p +%zu bytes)%s\n"
		"[abl-copy]   src =%p room=%zu (block %p +%zu bytes)%s\n",
		destBad && srcBad ? "BOTH ENDS" : (destBad ? "ON THE WRITE" : "on the read"),
		size, srcTypeSize,
		dest, destRoom, destBase, destBytes, destBad ? "  <== OVERRUN" : "",
		src,  srcRoom,  srcBase,  srcBytes,  srcBad  ? "  <== OVERRUN" : "");
	fflush(stderr);
}

void execAssignmentStatement (SymTableNodePtr idPtr) {

	StackItemPtr targetPtr;
	TypePtr targetTypePtr;
	TypePtr expressionTypePtr;

	//--------------------------
	// Assignment to variable...
	targetTypePtr = execVariable(idPtr, USE_TARGET);
	targetPtr = (StackItemPtr)tos->address;

	//------------------------------
	// Pop off the target address...
	pop();
	
	//------------------------
	// Pop the size, if nec...
	//if (targetTypePtr->form == FRM_ARRAY)
	//	pop();

	//---------------------------------------------------------------
	// Routine execExpression() leaves the expression value on top of
	// stack...
	getCodeToken();
	expressionTypePtr = execExpression();

	//--------------------------
	// Now, do the assignment...
	if ((targetTypePtr == RealTypePtr) && (expressionTypePtr == IntegerTypePtr)) {
		//-------------------------
		// integer assigned to real
		targetPtr->real = (float)(tos->integer);
		}
	else if (targetTypePtr->form == FRM_ARRAY) {
		//-------------------------
		// Copy the array/record...
		char* dest = (char*)targetPtr;
		char* src = tos->address;
		long size = targetTypePtr->size;
		// PORT: B1-1 census (M8-B7). The 2026-08-18 page-guard run faulted
		// READING past the source here: the copy length is bounded by neither
		// buffer, it is the declared type's size. A read cannot corrupt a heap,
		// but this same call WRITES `size` bytes to dest. Report every copy that
		// runs past a known allocation, on either end.
		MC2_AblCopyCensus(dest, src, size,
			expressionTypePtr ? (long)expressionTypePtr->size : -1);
		if (MC2_AblCopyClampEnabled() && expressionTypePtr &&
			(long)expressionTypePtr->size > 0 && (long)expressionTypePtr->size < size)
			size = (long)expressionTypePtr->size;
		memcpy(dest, src, size);
		}
	else if ((targetTypePtr == IntegerTypePtr) || (targetTypePtr->form == FRM_ENUM)) {
		//------------------------------------------------------
		// Range check assignment to integer or enum subrange...
		targetPtr->integer = tos->integer;
		}
	else if (targetTypePtr == CharTypePtr)
		targetPtr->byte = tos->byte;
	else {
		//-----------------------
		// Assign real to real...
		targetPtr->real = tos->real;
	}

	//-----------------------------
	// Grab the expression value...
	pop();

	if (debugger)
		debugger->traceDataStore(idPtr, idPtr->typePtr, targetPtr, targetTypePtr);
}

//***************************************************************************

TypePtr execRoutineCall (SymTableNodePtr routineIdPtr, bool skipOrder) {

	if (routineIdPtr->defn.info.routine.key == RTN_DECLARED)
		return(execDeclaredRoutineCall(routineIdPtr, skipOrder));
	else
		return(execStandardRoutineCall(routineIdPtr, skipOrder));
}

//***************************************************************************

TypePtr execDeclaredRoutineCall (SymTableNodePtr routineIdPtr, bool skipOrder) {

	if (skipOrder) {
		StackItemPtr curStackFrameBase = tos;
		//----------------------------------------
		// Push parameter values onto the stack...
		getCodeToken();
		if (codeToken == TKN_LPAREN) {
			execActualParams(routineIdPtr);
			getCodeToken();
		}
		getCodeToken();
		tos = curStackFrameBase;
		pushInteger(1);
		return((TypePtr)(routineIdPtr->typePtr));
	}

	long oldLevel = level;						// level of caller
	long newLevel = routineIdPtr->level + 1;	// level of callee
	CallStackLevel++;

	//-------------------------------------------
	// First, set up the stack frame of callee...
	StackItemPtr newStackFrameBasePtr = tos + 1;
	bool isLibraryCall = (routineIdPtr->library && (routineIdPtr->library != CurRoutineIdPtr->library));
	if (isLibraryCall)
		pushStackFrameHeader(-1, -1);
	else
		pushStackFrameHeader(oldLevel, newLevel);

	//----------------------------------------
	// Push parameter values onto the stack...
	getCodeToken();
	if (codeToken == TKN_LPAREN) {
		execActualParams(routineIdPtr);
		getCodeToken();
	}

	//-------------------------------------------------
	// Set the return address in the new stack frame...
	level = newLevel;
	stackFrameBasePtr = newStackFrameBasePtr;
	StackFrameHeaderPtr headerPtr = (StackFrameHeaderPtr)stackFrameBasePtr;
	headerPtr->returnAddress.address = codeSegmentPtr - 1;

	//---------------------------------------------------------
	// If we're calling a library function, we need to set some
	// module-specific info...
	ABLModulePtr PrevModule = NULL;
	if (isLibraryCall) {
		PrevModule = CurModule;
		CurModule = routineIdPtr->library;
		CurModuleHandle = CurModule->getHandle();
		if (debugger)
			debugger->setModule(CurModule);
		StaticDataPtr = CurModule->getStaticData();
		CallModuleInit = !CurModule->getInitCalled();
		CurModule->setInitCalled(true);
	//	routineEntry(ModuleRegistry[CurModule->getHandle()].moduleIdPtr);
	}

	if (ProfileLog) {
		long functionStartTime = ABLGetTimeCallback();
		execute(routineIdPtr);
		long functionExecTime = ABLGetTimeCallback() - functionStartTime;

		if (functionExecTime > ProfileLogFunctionTimeLimit) {
			char s[512];
			sprintf(s, "[%08d] ", NumExecutions);
			for (long i = 0; i < CallStackLevel; i++)
				strcat(s, " ");
			char s1[512];
			sprintf(s1, "%s (%d)\n", routineIdPtr->name, functionExecTime);
			strcat(s, s1);
			ABL_AddToProfileLog(s);
		}
		}
	else
		execute(routineIdPtr);

	//----------------------------------------------------------------
	// If we're calling a library function, reset some module-specific
	// info...
	if (isLibraryCall) {
	//	routineExit(ModuleRegistry[CurModule->getHandle()].moduleIdPtr);
		CurModule = PrevModule;
		CurModuleHandle = CurModule->getHandle();
		if (debugger)
			debugger->setModule(CurModule);
		StaticDataPtr = CurModule->getStaticData();
	}

	//-------------------------------------------------------
	// Return from the callee, and grab the first token after
	// the return...
	level = oldLevel;
	getCodeToken();
	CallStackLevel--;

	return((TypePtr)(routineIdPtr->typePtr));
}

//***************************************************************************

void setOpenArray (TypePtr arrayTypePtr, long size) {

	long numElements = size / arrayTypePtr->size;
	arrayTypePtr->size = size;
	while (arrayTypePtr->info.array.elementTypePtr->form == FRM_ARRAY)
		arrayTypePtr = arrayTypePtr->info.array.elementTypePtr;
	arrayTypePtr->info.array.elementCount = numElements;
}

//***************************************************************************

void execActualParams (SymTableNodePtr routineIdPtr) {

	//--------------------------
	// Execute the parameters...

	for (SymTableNodePtr formalIdPtr = (SymTableNodePtr)(routineIdPtr->defn.info.routine.params);
		 formalIdPtr != NULL;
		 formalIdPtr = formalIdPtr->next) {

		TypePtr formalTypePtr = (TypePtr)(formalIdPtr->typePtr);
		getCodeToken();

		if (formalIdPtr->defn.key == DFN_VALPARAM) {
			//-------------------
			// pass by value parameter...
			TypePtr actualTypePtr = execExpression();

			if ((formalTypePtr == RealTypePtr) && (actualTypePtr == IntegerTypePtr)) {
				//---------------------------------------------
				// Real formal parameter, but integer actual...
				tos->real = (float)(tos->integer);
			}

			//----------------------------------------------------------
			// Formal parameter is an array or record, so make a copy...
			if ((formalTypePtr->form == FRM_ARRAY)/* || (formalTypePtr->form == FRM_RECORD)*/) {
				//------------------------------------------------------------------------------
				// The following is a little inefficient, but is kept this way to keep it clear.
				// Once it's verified to work, optimize...

				long size = formalTypePtr->size;
				char* src = tos->address;
				char* dest = (char*)ABLStackMallocCallback((size_t)size);
				if (!dest) {
					char err[255];
					sprintf(err, " ABL: Unable to AblStackHeap->malloc actual array param in module %s)", CurModule->getName());
					ABL_Fatal(0, err);
				}
				char* savePtr = dest;
				memcpy(dest, src, size);
				tos->address = savePtr;
			}
			}
		else {
			//-------------------------------
			// pass by reference parameter...
			SymTableNodePtr idPtr = getCodeSymTableNodePtr();
			execVariable(idPtr, USE_REFPARAM);
		}
	}
}

//***************************************************************************

void execSwitchStatement (void) {

	getCodeToken();
		
	char* branchTableLocation = getCodeAddressMarker();

	getCodeToken();
	TypePtr switchExpressionTypePtr = execExpression();

	long switchExpressionValue;
	if ((switchExpressionTypePtr == IntegerTypePtr) || (switchExpressionTypePtr->form == FRM_ENUM))
		switchExpressionValue = tos->integer;
	else
		switchExpressionValue = tos->byte;
	pop();

	//---------------------------------------------------------
	// Now, search the branch table for the expression value...
	codeSegmentPtr = branchTableLocation;
	getCodeToken();
	long caseLabelCount = getCodeInteger();
	bool done = false;
	char* caseBranchLocation = NULL;
	while (!done && caseLabelCount--) {
		long caseLabelValue = getCodeInteger();
		caseBranchLocation = getCodeAddress();
		done = (caseLabelValue == switchExpressionValue);
	}

	//-----------------------------------------------
	// If found, go to the aprropriate branch code...
	if (caseLabelCount >= 0) {
		codeSegmentPtr = caseBranchLocation;
		getCodeToken();

		if (codeToken != TKN_END_CASE)
			do {
				execStatement();
				if (ExitWithReturn)
					return;
			} while (codeToken != TKN_END_CASE);
		

		//----------------------------------
		// Grab the end case and semi-colon...
		getCodeToken();
		getCodeToken();
		codeSegmentPtr = getCodeAddressMarker();
		getCodeToken();
		}
	else {
		//-----------------------------------------------------------------
		// Since the branch table is located at the end of the case blocks,
		// the code directly after the switch statement follows our
		// current code location, already. Just grab the endswitch
		// and semi-colon...
		getCodeToken();
		getCodeToken();
	}
}

//***************************************************************************

void execForStatement (void) {

	getCodeToken();

	//---------------------------------------
	// Grab address of the end of the loop...
	char* loopEndLocation = getCodeAddressMarker();

	//--------------------------------------------------------
	// Get the address of the control variable's stack item...
	getCodeToken();
	SymTableNodePtr controlIdPtr = getCodeSymTableNodePtr();
	TypePtr controlTypePtr = execVariable(controlIdPtr, USE_TARGET);
	StackItemPtr targetPtr = (StackItemPtr)tos->address;
	
	//------------------------------------
	// Control variable address...
	pop();

	//-------------------------------
	// Eval the initial expression...
	getCodeToken();
	execExpression();
	long initialValue;
	if (controlTypePtr == IntegerTypePtr)
		initialValue = tos->integer;
	else
		initialValue = tos->byte;
	//---------------------
	// The initial value...
	pop();

	long deltaValue;
	if (codeToken == TKN_TO)
		deltaValue = 1;
	else
		deltaValue = -1;

	//----------------------------------
	// Now, eval the final expression...
	getCodeToken();
	execExpression();
	long finalValue;
	if (controlTypePtr == IntegerTypePtr)
		finalValue = tos->integer;
	else
		finalValue = tos->byte;
	//-------------------
	// The final value...
	pop();

	//----------------------------
	// Address of start of loop...
	char* loopStartLocation = codeSegmentPtr;

	long controlValue = initialValue;

	//-----------------------------
	// Now, execute the FOR loop...
	long iterations = 0;
	if (deltaValue == 1)
		while (controlValue <= finalValue) {
			if (controlTypePtr == IntegerTypePtr)
				targetPtr->integer = controlValue;
			else
				targetPtr->byte = (unsigned char)controlValue;

			getCodeToken();

			if (codeToken != TKN_END_FOR)
				do {
					execStatement();
					if (ExitWithReturn)
						return;
				} while (codeToken != TKN_END_FOR);

			//---------------------------
			// Check for infinite loop...
			if (++iterations == MaxLoopIterations)
				runtimeError(ABL_ERR_RUNTIME_INFINITE_LOOP);

			controlValue++;
			codeSegmentPtr = loopStartLocation;
		}
	else
		while (controlValue >= finalValue) {
			if (controlTypePtr == IntegerTypePtr)
				targetPtr->integer = controlValue;
			else
				targetPtr->byte = (unsigned char)controlValue;

			getCodeToken();

			if (codeToken != TKN_END_FOR)
				do {
					execStatement();
					if (ExitWithReturn)
						return;
				} while (codeToken != TKN_END_FOR);

			//---------------------------
			// Check for infinite loop...
			if (++iterations == MaxLoopIterations)
				runtimeError(ABL_ERR_RUNTIME_INFINITE_LOOP);
			
			controlValue--;
			codeSegmentPtr = loopStartLocation;
		}

	codeSegmentPtr = loopEndLocation;
	getCodeToken();
}

//***************************************************************************

void execTransStatement (void) {

	getCodeToken();
	getCodeToken();
	SymTableNodePtr idPtr = getCodeSymTableNodePtr();
	transState(idPtr);
	getCodeToken();
}

//***************************************************************************

void execTransBackStatement (void) {

	SymTableNodePtr prevState = CurModule->getPrevState();
	if (!prevState)
		runtimeError(ABL_ERR_RUNTIME_NULL_PREVSTATE);
	transState(prevState);
	getCodeToken();
}

//***************************************************************************

void execIfStatement (void) {

	getCodeToken();

	char* falseLocation = getCodeAddressMarker();

	//-------------------------------
	// Eval the boolean expression. Note that, unlike C/C++, the expression
	// must be true(1) or false(0). In C/C++, an expression is true if it's
	// non-zero. Not the case in ABL using this current implementation. Do we
	// want to change this?
	getCodeToken();
	execExpression();
	bool test = (tos->integer == 1);
	pop();

	if (test) {
		//---------------------------
		// execute the TRUE branch...
		getCodeToken();

		if ((codeToken != TKN_END_IF) && (codeToken != TKN_ELSE))
			do {
				execStatement();
				if (ExitWithReturn)
					return;
			} while ((codeToken != TKN_END_IF) && (codeToken != TKN_ELSE));
		
		if (codeToken == TKN_ELSE) {
			getCodeToken();
			codeSegmentPtr = getCodeAddressMarker();
			getCodeToken();
		}
		}
	else {
		//----------------------------
		// Execute the FALSE branch...
		codeSegmentPtr = falseLocation;
		getCodeToken();

		if (codeToken == TKN_ELSE) {
			getCodeToken();
			getCodeAddressMarker();

			getCodeToken();

			if (codeToken != TKN_END_IF)
				do {
					execStatement();
					if (ExitWithReturn)
						return;
				} while (codeToken != TKN_END_IF);
		}
	}
	getCodeToken();
}

//***************************************************************************

void execRepeatStatement (void) {

	char* loopStartLocation = codeSegmentPtr;

	long iterations = 0;
	do {
		getCodeToken();

		if (codeToken != TKN_UNTIL)
			do {
				execStatement();
				if (ExitWithReturn)
					return;
			} while (codeToken != TKN_UNTIL);

		//---------------------------
		// Check for infinite loop...
		iterations++;
		if (iterations == MaxLoopIterations)
			runtimeError(ABL_ERR_RUNTIME_INFINITE_LOOP);

		//-------------------------------
		// Eval the boolean expression...
		getCodeToken();
		execExpression();
		if (tos->integer == 0)
			codeSegmentPtr = loopStartLocation;

		//--------------------------
		// Grab the boolean value...
		pop();
	} while (codeSegmentPtr == loopStartLocation);
}

//***************************************************************************

void execWhileStatement (void) {

	getCodeToken();
	char* loopEndLocation = getCodeAddressMarker();
	char* testLocation = codeSegmentPtr;

	bool loopDone = false;
	long iterations = 0;
	do {
		//-------------------------------
		// Eval the boolean expression...
		getCodeToken();
		execExpression();
		if (tos->integer == 0) {
			codeSegmentPtr = loopEndLocation;
			loopDone = true;
		}
		
		//-------------------------
		// Get the boolean value...
		pop();

		//----------------------------------
		// If TRUE, execute the statement...
		if (!loopDone) {
			getCodeToken();

			if (codeToken != TKN_END_WHILE)
				do {
					execStatement();
					if (ExitWithReturn)
						return;
				} while (codeToken != TKN_END_WHILE);

			codeSegmentPtr = testLocation;

			//---------------------------
			// Check for infinite loop...
			iterations++;
			if (iterations == MaxLoopIterations)
				runtimeError(ABL_ERR_RUNTIME_INFINITE_LOOP);
		}
	} while (!loopDone);

	getCodeToken();
}

//***************************************************************************
