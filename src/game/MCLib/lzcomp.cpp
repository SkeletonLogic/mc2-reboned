//--------------------------------------------------------------------------
// LZ Compress Routine
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef _MBCS
#include <GameOS.HPP> // PORT: match case-sensitive host path.

#ifndef HEAP_H
#include "Heap.h" // PORT: match case-sensitive host path.
#endif

#else
#include <assert.h>
#include <malloc.h>
#define gosASSERT assert
#define gos_Malloc malloc
#define gos_Free free
#endif

//---------------------------------------------------------------------------
// Static Globals

#ifndef NULL
#define NULL			0
#endif

typedef unsigned char* MemoryPtr;

//-----------------------------
//Used by Compressor Routine
MemoryPtr		LZCHashBuf = NULL;

unsigned long 	InBufferUpperLimit = 0;
unsigned long 	InBufferPos = 0;	
MemoryPtr		InBuffer = NULL;

unsigned long 	OutBufferPos = 0;
MemoryPtr 		OutBuffer = NULL;

unsigned long 	PrefixCode = 0;
unsigned long 	FreeCode = 0;
unsigned long 	MaxCode = 0;
unsigned long 	NBits = 0;
unsigned long 	BitOffset = 0;
unsigned char 	K = 0;

unsigned long	codeToWrite = 0;

#define MaxMax		4096
#define Clear		256
#define EOF			257
#define First_Free	258

struct Hash
{
	unsigned long hashFirst;
	unsigned long hashNext;
	unsigned char hashChar;
};

static unsigned char		tag_LZCHashBuf[sizeof(Hash) * MaxMax + 1024];
//-----------------------------

//-------------------------------------------------------------------------------
// PORT: the body below was 25 blocks of inline x86 assembly. Inline __asm is
// unavailable on every 64-bit toolchain -- MSVC rejects it on x64 as well, so
// this is not a clang-only limitation -- and unlike Heap.cpp's assembly
// allocator there is no alternative path to fall back on: LZCompress has 8
// runtime callers including LogisticsData.cpp:1971 (SAVE-GAME WRITING),
// TerrTxm.cpp:1330, txmmgr.cpp:1135/1283, ffile.cpp x3 and File.cpp:1074.
// It had to be genuinely ported.
//
// THE ALGORITHM IS LZW, transcribed block by block from the assembly:
//   * codes start at 9 bits and grow to 12 (MaxMax 4096)
//   * 256 = Clear, 257 = EOF, first assignable code is 258
//   * the hash table is a chained (prefix -> suffix) trie: hashFirst is the
//     head of a prefix's child chain, hashNext walks siblings, hashChar is the
//     suffix byte. The assembly indexed it with `lea esi,[ebx*8]; add esi,ebx`
//     -- i.e. 9-byte entries -- but the table is purely transient working
//     state, never serialised, so a natural struct array is used here instead.
//
// OUTPUT MUST BE BIT-IDENTICAL. lzdecomp.cpp is unchanged and still reads this
// exact bitstream, and .txm texture caches and save games written by one are
// read by the other. The bit packer below therefore reproduces the assembly's
// writeCode/AdvanceBuffer precisely: each code is deposited as a little-endian
// 24-bit value at the current byte position, pre-shifted left by the running
// bit offset and OR-ed into the partial byte already there (the assembly's
// `jecxz` skipped both the shift and the OR when the offset was zero, which is
// why the OR below is conditional).
//
// 64-BIT NOTE: the original stored raw pointers in the `unsigned long` globals
// InBufferPos/OutBufferPos. That silently truncates on x64, where unsigned long
// is still 32 bits on Windows. They now carry byte OFFSETS, which is both
// 64-bit safe and what the surrounding arithmetic actually wanted.
//-------------------------------------------------------------------------------

struct LZCEntry
{
	unsigned long	first;		// head of this prefix's child chain, -1 if none
	unsigned long	next;		// next sibling, -1 if end of chain
	unsigned char	ch;			// suffix byte
};

static const unsigned long LZC_UNUSED = 0xFFFFFFFFul;

//
// Deposits `code` at the current output position using the original's packing.
// Always writes three bytes, exactly as `stosw` + `stosb` did; callers size the
// destination buffer for that, and the trailing bytes past the returned length
// are ignored.
//
static void LZC_WriteCode(MemoryPtr out, unsigned long& bytePos,
						  unsigned long& bitOffset, unsigned long nBits,
						  unsigned long code)
{
	unsigned char* p = out + bytePos;

	unsigned long v = code << bitOffset;
	if (bitOffset)
		v |= p[0];					// merge with the partially filled byte

	p[0] = (unsigned char)( v        & 0xFF);
	p[1] = (unsigned char)((v >>  8) & 0xFF);
	p[2] = (unsigned char)((v >> 16) & 0xFF);

	// AdvanceBuffer: step on by whole bytes, keeping the leftover bit count.
	unsigned long t = bitOffset + nBits;
	bytePos   += (t >> 3);
	bitOffset  = (t & 7);
}

long LZCompress (MemoryPtr dest, MemoryPtr src, unsigned long srcLen)
{
	long result = 0;

	if (!LZCHashBuf)
	{
		/* allocating LZCHashBuf off a gos heap causes problems for applications that need
		to reset gos or its heaps*/
		LZCHashBuf = (MemoryPtr)&(tag_LZCHashBuf[0]);
	}

	LZCEntry* table = (LZCEntry*)LZCHashBuf;

	InBuffer            = src;
	InBufferPos         = 0;
	InBufferUpperLimit  = srcLen;
	OutBuffer           = dest;
	OutBufferPos        = 0;
	BitOffset           = 0;

	// InitTable. The assembly cleared only the low entries; every higher entry
	// is initialised by AddCode before it can be read, so clearing the whole
	// table here is equivalent and removes the need to reason about it.
	NBits    = 9;
	MaxCode  = 512;
	FreeCode = First_Free;
	for (int i = 0; i < MaxMax; ++i)
	{
		table[i].first = LZC_UNUSED;
		table[i].next  = LZC_UNUSED;
		table[i].ch    = 0;
	}

	LZC_WriteCode(dest, OutBufferPos, BitOffset, NBits, Clear);

	// The original's behaviour on an empty input was undefined -- it read a
	// character that was never loaded. Emitting just Clear + EOF is the
	// well-defined equivalent.
	if (srcLen == 0)
	{
		LZC_WriteCode(dest, OutBufferPos, BitOffset, NBits, EOF);
		if (BitOffset) OutBufferPos++;
		return (long)OutBufferPos;
	}

	unsigned long ch = src[InBufferPos++];

	for (;;)
	{
		// Make_Into_Code / Set_AX_Prefix
		PrefixCode = ch;

		for (;;)
		{
			if (InBufferPos >= InBufferUpperLimit)
				goto FoundEOF;

			unsigned long suffix = src[InBufferPos++];
			K = (unsigned char)suffix;

			// LookUpCode: walk the prefix's child chain looking for `suffix`.
			unsigned long parent  = PrefixCode;
			bool          hadChain = false;
			unsigned long slot     = parent;	// entry whose link we may update
			bool          found    = false;

			if (table[parent].first != LZC_UNUSED)
			{
				hadChain = true;
				unsigned long node = table[parent].first;
				for (;;)
				{
					slot = node;
					if (table[node].ch == (unsigned char)suffix)
					{
						PrefixCode = node;		// extend the match
						found = true;
						break;
					}
					if (table[node].next == LZC_UNUSED)
						break;					// end of chain, no match
					node = table[node].next;
				}
			}

			if (found)
				continue;						// keep growing the prefix

			// AddCode: assign the next free code to (PrefixCode, suffix).
			unsigned long assigned = FreeCode;

			if (hadChain) table[slot].next  = assigned;
			else          table[parent].first = assigned;

			if (assigned != MaxMax)
			{
				table[assigned].first = LZC_UNUSED;
				table[assigned].next  = LZC_UNUSED;
				table[assigned].ch    = (unsigned char)suffix;
				FreeCode = assigned + 1;
			}
			else
			{
				FreeCode = assigned;
			}

			LZC_WriteCode(dest, OutBufferPos, BitOffset, NBits, PrefixCode);

			ch = K;

			if (assigned < MaxCode)
				break;							// back to Make_Into_Code

			if (NBits < 12)
			{
				NBits++;
				MaxCode <<= 1;
				break;
			}

			// All 12-bit codes used: emit Clear and restart the table.
			LZC_WriteCode(dest, OutBufferPos, BitOffset, NBits, Clear);

			NBits    = 9;
			MaxCode  = 512;
			FreeCode = First_Free;
			for (int i = 0; i < MaxMax; ++i)
			{
				table[i].first = LZC_UNUSED;
				table[i].next  = LZC_UNUSED;
				table[i].ch    = 0;
			}

			ch = K;
			break;
		}
	}

FoundEOF:
	LZC_WriteCode(dest, OutBufferPos, BitOffset, NBits, PrefixCode);
	LZC_WriteCode(dest, OutBufferPos, BitOffset, NBits, EOF);
	if (BitOffset)
		OutBufferPos++;

	result = (long)OutBufferPos;
	return result;
}

#if MC2_X86_ASM_LZCOMP
// Original x86 assembly implementation, preserved for reference only.
long LZCompress_x86asm (MemoryPtr dest, MemoryPtr src, unsigned long srcLen)
{
	long result = 0;

	if (!LZCHashBuf)
	{
		LZCHashBuf = (MemoryPtr)&(tag_LZCHashBuf[0]);
	}

//Initialize:
	
	unsigned long clearSize = sizeof(Hash) * 256;

	__asm
	{
		mov		eax,[dest]
		mov		[OutBufferPos],eax
		mov		[OutBuffer],eax
		mov		eax,[src]
		mov		[InBufferPos],eax
		mov		[InBuffer],eax
		add		eax,[srcLen]
		mov		[InBufferUpperLimit],eax

		mov		[BitOffset],0         		//zero out BitOffset
		
		//call	InitTable
		mov		[NBits],9          			//Starting with 9 bit codes
		mov		[MaxCode],512      			//10 0000 0000 b
		mov		eax,-1              		//Marked as unused
		mov		ecx,[clearSize]				//Clear first 256 entries
		mov		edi,[LZCHashBuf]   			//Start at the start of buffer
		rep		stosb
		mov		[FreeCode],First_Free 		//Next code to use is first_free

		mov		eax,Clear             		//first byte in buffer or file is CLEAR
		
		//call	writeCode
		xor		edx,edx					//make sure the DL is CLEAR
		mov		edi,[OutBufferPos]   	//obtain destination address
		mov		ecx,[BitOffset]        	//get bitposition
		jecxz	save1
	}
		
shift1:

	__asm
	{
		shl		ax,1
		rcl		edx,1
		loop	shift1
		or		al,[edi]
	}
	
save1:

	__asm
	{
		stosw
		mov		al,dl
		stosb
		
		//AdvanceBuffer
		
		mov		ecx,[NBits]                 ;get number of bits to advance
		mov		eax,[BitOffset]             ;get low word of OutBuffer
		add		eax,ecx
		mov		cl,al
		shr		al,3
		add		[OutBufferPos],eax
		and		cl,7
		movzx	ecx,cl
		mov		[BitOffset],ecx

//-------------------------------------------------------------------------
// This is the main compression loop                                       
//-------------------------------------------------------------------------

		//call	ReadChar              		//get byte from source
		mov		esi,[InBufferPos]    		//get address
   		cmp		esi,[InBufferUpperLimit]	//Check to see if we are done
		cmc									//compliment carry
		jc		doneRC1

		lodsb                       		//load byte
		mov		[InBufferPos],esi
		clc
	}
	
doneRC1:
		
Make_Into_Code:

	__asm
	{
		movzx	eax,al                 		//turn char into code
	}
		
//-------------------------------------------------------------------------
Set_AX_Prefix:

	__asm
	{
		mov		[PrefixCode],eax       		//into prefix code
		
		//call	ReadChar              		//more...
		mov		esi,[InBufferPos]    		//get address
   		cmp		esi,[InBufferUpperLimit]	//Check to see if we are done
		cmc									//compliment carry
		jc		doneRC2

		lodsb                       		//load byte
		mov		[InBufferPos],esi
		clc
	}
		
doneRC2:

	__asm
	{
		jc		FoundEOF            		//No more input
		mov		[K],al                		//Save returned char
		mov		ebx,[PrefixCode]       		//check for this pair
		//call	LookUpCode            		//in the table

		//call	Index                      	//index into current table address
		lea		esi,[ebx*8]
		add		esi,ebx            			//EBX * 9
		add		esi,[LZCHashBuf]
		
		xor		edi,edi                     //flag destination
		cmp		[esi].hashFirst,-1 			//see if code is used
		je		short exit1            		//if == then CARRY = CLEAR, set it    
		inc		edi                        	//flag as used
		mov		ebx,[esi].hashFirst			//get prefix code
	}
		
lookloop:

	__asm
	{
		//call	Index                      	//translate prefix or table to index
		lea		esi,[ebx*8]
		add		esi,ebx            			//EBX * 9
		add		esi,[LZCHashBuf]
		
		cmp		[esi].hashChar,al    		//is the suffix the same? if yes
		jne		notSame          			//then we can compress this a
		clc                              	//little more by taking this prefix
		mov		eax,ebx                     //code and getting a new suffix
		jmp		short exit2               	//in a moment
	}

notSame:

	__asm
	{
		cmp		[esi].hashNext,-1 			//found a new pattern so exit
		je		exit1                    	//if == then CARRY = CLEAR, set it
		mov		ebx,[esi].hashNext 			//continue through chain to get to
		jmp		short lookloop             	//end of chain
	}
	
exit1:
	__asm
	{
		stc
	}
	
exit2:

	__asm
	{
		jnc		Set_AX_Prefix       		//new prefix in EAX
		
//------------------------------------------------------------------------

		//call	AddCode               		//Add to table
		mov		ebx,[FreeCode]         		//get the next available hash table code
		push	ebx
		or		edi,edi                 	//is this first use of prefix?
		je		short newprefix
		mov		[esi].hashNext,ebx 			//if not, then set next code dest
		jmp		short addcode
	}
		
newprefix:

	__asm
	{
		mov		[esi].hashFirst,ebx			//Mark first as used
	}
		
addcode:

	__asm
	{
		cmp		ebx,MaxMax                  //is this the last code?
		je		exitAC

		//call	Index                     	//create new entry
		lea		esi,[ebx*8]
		add		esi,ebx            			//EBX * 9
		add		esi,[LZCHashBuf]
			
		mov		[esi].hashFirst,-1 			//mark new entry as unused
		mov		[esi].hashNext,-1
		mov		[esi].hashChar,al  			//and save suffix
		inc		ebx                         //set up for next code available
	}
		
exitAC:

	__asm
	{
		mov	[FreeCode],ebx
		pop	ebx
	}

	__asm
	{
		push	ebx                    		//save new code
		mov		eax,[PrefixCode]       		//write the old prefix code
		
		//call	writeCode
		xor		edx,edx					//make sure the DL is CLEAR
		mov		edi,[OutBufferPos]   	//obtain destination address
		mov		ecx,[BitOffset]        	//get bitposition
		jecxz	save5
	}
		
shift5:

	__asm
	{
		shl		ax,1
		rcl		edx,1
		loop	shift5
		or		al,[edi]
	}
	
save5:

	__asm
	{
		stosw
		mov		al,dl
		stosb
		
		//AdvanceBuffer
		
		mov		ecx,[NBits]                 ;get number of bits to advance
		mov		eax,[BitOffset]             ;get low word of OutBuffer
		add		eax,ecx
		mov		cl,al
		shr		al,3
		add		[OutBufferPos],eax
		and		cl,7
		movzx	ecx,cl
		mov		[BitOffset],ecx
		
		pop		ebx                    		//back
		mov		al,[K]
		cmp		ebx,[MaxCode]          		//exceeded size?
		jb		Make_Into_Code        		//no
		cmp		[NBits],12            		//less than 12 bit encoding?
		jb		Another_Bit           		//give it one more

//---------------------------------------------------------------
// All codes up to 12 bits are used: clear table and restart
//---------------------------------------------------------------

		mov		eax,Clear             	//write a clear code.....out of bits
		
		//call writeCode
		xor		edx,edx					//make sure the DL is CLEAR
		mov		edi,[OutBufferPos]   	//obtain destination address
		mov		ecx,[BitOffset]        	//get bitposition
		jecxz	save2
	}
		
shift2:

	__asm
	{
		shl		ax,1
		rcl		edx,1
		loop	shift2
		or		al,[edi]
	}
	
save2:

	__asm
	{
		stosw
		mov		al,dl
		stosb
		
		//AdvanceBuffer
		
		mov		ecx,[NBits]                 ;get number of bits to advance
		mov		eax,[BitOffset]             ;get low word of OutBuffer
		add		eax,ecx
		mov		cl,al
		shr		al,3
		add		[OutBufferPos],eax
		and		cl,7
		movzx	ecx,cl
		mov		[BitOffset],ecx
		
		//call	InitTable             		//Cleanup table
		mov		[NBits],9          			//Starting with 9 bit codes
		mov		[MaxCode],512      			//10 0000 0000 b
		mov		eax,-1              		//Marked as unused
		mov		ecx,clearSize				//Clear first 256 entries
		mov		edi,[LZCHashBuf]   			//Start at the start of buffer
		rep		stosb
		mov		[FreeCode],First_Free 		//Next code to use is first_free

		
		mov		al,[K]                		//Last char
		jmp		Make_Into_Code
	}

//---------------------------------------------------------------
// Extended bit length by a bit, adjusting Max_Code accordingly
//---------------------------------------------------------------
Another_Bit:

	__asm
	{
		inc		[NBits]               		//one more
		shl		[MaxCode],1           		//Double it's size
		jmp		Make_Into_Code
	}

//----------------------------------------------------------------
// No more input: flush what's left and perform housekeeping
//----------------------------------------------------------------

FoundEOF:

	__asm
	{
		mov		eax,[PrefixCode]			//write the last code
		
		//call	writeCode
		xor		edx,edx					//make sure the DL is CLEAR
		mov		edi,[OutBufferPos]   	//obtain destination address
		mov		ecx,[BitOffset]        	//get bitposition
		jecxz	save3
	}
		
shift3:

	__asm
	{
		shl		ax,1
		rcl		edx,1
		loop	shift3
		or		al,[edi]
	}
	
save3:

	__asm
	{
		stosw
		mov		al,dl
		stosb
		
		//AdvanceBuffer
		
		mov		ecx,[NBits]                 ;get number of bits to advance
		mov		eax,[BitOffset]             ;get low word of OutBuffer
		add		eax,ecx
		mov		cl,al
		shr		al,3
		add		[OutBufferPos],eax
		and		cl,7
		movzx	ecx,cl
		mov		[BitOffset],ecx
		
		mov		eax,EOF               		//write EOF code
		
		//call	writeCode
		xor		edx,edx					//make sure the DL is CLEAR
		mov		edi,[OutBufferPos]   	//obtain destination address
		mov		ecx,[BitOffset]        	//get bitposition
		jecxz	save4
	}
		
shift4:

	__asm
	{
		shl		ax,1
		rcl		edx,1
		loop	shift4
		or		al,[edi]
	}
	
save4:

	__asm
	{
		stosw
		mov		al,dl
		stosb
		
		//AdvanceBuffer
		
		mov		ecx,[NBits]                 ;get number of bits to advance
		mov		eax,[BitOffset]             ;get low word of OutBuffer
		add		eax,ecx
		mov		cl,al
		shr		al,3
		add		[OutBufferPos],eax
		and		cl,7
		movzx	ecx,cl
		mov		[BitOffset],ecx
		
		mov		eax,[BitOffset]       		//bits waiting to go?
		or		eax,eax
		je		CompressDone       			//no....just close things up and go back
		inc		[OutBufferPos]
	}
	
CompressDone:

	__asm
	{
		//	return number of bytes in compressed buffer
		mov	eax,[OutBufferPos]
		sub	eax,[OutBuffer]
		
		mov [result],eax
	}

	return(result);
}
#endif // MC2_X86_ASM_LZCOMP

