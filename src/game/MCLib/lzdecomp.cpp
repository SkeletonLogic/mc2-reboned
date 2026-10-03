//--------------------------------------------------------------------------
// LZ Decompress Routine
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Static Globals

#define HASH_CLEAR		256               //clear hash table command code
#define HASH_EOF        257               //End Of Data command code
#define HASH_FREE       258               //First Hash Table Chain Offset Value

#define BASE_BITS       9
#define MAX_BIT_INDEX   (1 << BASE_BITS)
#define NO_RAM_FOR_LZ_DECOMP	0xCBCB0002

#ifndef NULL
#define NULL			0
#endif

typedef unsigned char* MemoryPtr;

struct HashStruct
{
	unsigned short Chain;
	unsigned char Suffix;
};

typedef HashStruct *HashStructPtr;
	
HashStructPtr	LZOldChain = NULL;			//Old Chain Value Found
HashStructPtr	LZChain = NULL;				//Current Chain Value Found 
unsigned long	LZMaxIndex = 0;				//Max index value in Hash Table
unsigned long	LZCodeMask = 0;
unsigned long	LZFreeIndex = 0;			//Current Free index into Hash Table
MemoryPtr		LZSrcBufEnd = NULL;			//ptr to 3rd from last byte in src buffer
MemoryPtr		LZOrigDOSBuf = NULL;		//original offset to start of src buffer
char			LZHashBuffer[16384];
char			LZOldSuffix = 0;			//Current Suffix Value found


//-----------------------------

//-------------------------------------------------------------------------------
// PORT: this was a single large x86 assembly block, and it is the counterpart to
// the LZCompress just ported in lzcomp.cpp -- the two must agree bit for bit.
//
// WHY IT HAD TO BE PORTED EVEN THOUGH IT COMPILED: clang-cl supports MS-style
// inline assembly on x86-64, so unlike lzcomp.cpp this file built cleanly and
// produced an object file. It was still broken. The prologue did
//     mov esi,src      mov edi,dest      lea ebx,[ebx+esi-3]
// on 64-bit pointers, silently truncating every one of them to 32 bits. That is
// far more dangerous than a compile error, and it sits on the .txm texture-cache
// and save-game READ path. Asm that compiles is not asm that works.
//
// ALGORITHM (LZW, mirroring lzcomp.cpp):
//   * 9-bit codes growing to 12; 256 = Clear, 257 = EOF, 258 = first free code
//   * each table entry is (Chain, Suffix): the predecessor code plus one byte
//   * a code is expanded by walking Chain backwards to a terminal literal
//     (code < 256), collecting suffixes, then emitting them in reverse -- the
//     assembly used the machine stack for exactly this, pushing while walking
//     and popping while writing
//   * the KwKwK case (code == FreeIndex, not yet in the table) is handled by
//     synthesising the entry from the previous code plus the previous suffix
//   * a new entry (OldChain, first char just emitted) is added after each string
//
// The 3-byte entry stride is preserved by indexing LZHashBuffer manually: the
// assembly addressed entries as `LZHashBuffer + eax*3` (lea [buf+eax+eax*2]),
// whereas sizeof(HashStruct) is 4 once padded, so a plain array would silently
// change the layout.
//
// Two latent faults in the original are fixed rather than reproduced: the source
// bounds test permitted a 4-byte read starting 3 bytes from the end (a 1-byte
// over-read), and the chain walk pushed onto the real stack with no depth limit,
// so a corrupt stream could smash it. Both are bounded here.
//-------------------------------------------------------------------------------

// 3-byte packed accessors over LZHashBuffer, matching the assembly's stride.
static inline unsigned short LZH_GetChain(unsigned long i)
{
	const unsigned char* p = (const unsigned char*)LZHashBuffer + (i * 3);
	return (unsigned short)(p[0] | (p[1] << 8));
}
static inline void LZH_SetChain(unsigned long i, unsigned short v)
{
	unsigned char* p = (unsigned char*)LZHashBuffer + (i * 3);
	p[0] = (unsigned char)(v & 0xFF);
	p[1] = (unsigned char)((v >> 8) & 0xFF);
}
static inline unsigned char LZH_GetSuffix(unsigned long i)
{
	return ((const unsigned char*)LZHashBuffer)[(i * 3) + 2];
}
static inline void LZH_SetSuffix(unsigned long i, unsigned char v)
{
	((unsigned char*)LZHashBuffer)[(i * 3) + 2] = v;
}

#define LZD_MAX_CODES	4096
#define LZD_STACK_MAX	(LZD_MAX_CODES + 16)

long LZDecomp (MemoryPtr dest, MemoryPtr src, unsigned long srcLen)
{
	unsigned char*			out       = dest;
	const unsigned char*	in        = src;
	unsigned long			inPos     = 0;
	unsigned long			bitOffset = 0;
	unsigned long			nBits     = BASE_BITS;

	// The assembly's end marker was src+srcLen-3, tested with `ja`, so reading
	// stopped once the position passed it.
	const unsigned long		srcLimit  = (srcLen >= 3) ? (srcLen - 3) : 0;

	unsigned char			chainStack[LZD_STACK_MAX];

	LZCodeMask  = MAX_BIT_INDEX - 1;
	LZMaxIndex  = MAX_BIT_INDEX;
	LZFreeIndex = HASH_FREE;
	LZOldSuffix = 0;
	LZOldChain  = NULL;
	LZChain     = NULL;

	unsigned long oldChain  = 0;
	unsigned long oldSuffix = 0;

	// Reads one code, LSB-first, exactly as the assembly did: load 32 bits at
	// the current byte, shift down by the running bit offset, mask to nBits.
	// Assembled byte-wise and clamped so the tail cannot over-read the buffer.
	#define LZD_READ_CODE(dst)                                               \
		do {                                                                 \
			if (inPos > srcLimit) goto done;                                 \
			unsigned long _v = 0;                                            \
			for (int _b = 0; _b < 4; ++_b) {                                 \
				unsigned long _i = inPos + (unsigned long)_b;                \
				if (_i < srcLen) _v |= ((unsigned long)in[_i]) << (_b * 8);  \
			}                                                                \
			(dst) = (_v >> bitOffset) & LZCodeMask;                          \
			unsigned long _t = bitOffset + nBits;                            \
			inPos    += (_t >> 3);                                           \
			bitOffset = (_t & 7);                                            \
		} while (0)

	for (;;)
	{
		unsigned long code;
		LZD_READ_CODE(code);

		if (code == HASH_EOF)
			goto done;

		if (code == HASH_CLEAR)
		{
			// Reset to 9-bit codes, then take the following code as a literal.
			nBits       = BASE_BITS;
			LZCodeMask  = MAX_BIT_INDEX - 1;
			LZMaxIndex  = MAX_BIT_INDEX;
			LZFreeIndex = HASH_FREE;

			unsigned long first;
			LZD_READ_CODE(first);

			oldChain  = first;
			oldSuffix = first & 0xFF;
			*out++    = (unsigned char)oldSuffix;
			continue;
		}

		// Expand `code` into the stack, walking Chain back to a literal.
		unsigned long depth = 0;
		unsigned long cur   = code;

		if (cur >= LZFreeIndex)
		{
			// KwKwK: the code names the entry about to be created, so its first
			// byte is the previous string's first byte.
			if (depth < LZD_STACK_MAX) chainStack[depth++] = (unsigned char)oldSuffix;
			LZH_SetSuffix(cur, (unsigned char)oldSuffix);
			LZH_SetChain(cur, (unsigned short)oldChain);
			cur = oldChain;
		}

		while (cur > 0xFF)
		{
			if (depth >= LZD_STACK_MAX) goto done;	// corrupt stream; bail out
			chainStack[depth++] = LZH_GetSuffix(cur);
			cur = LZH_GetChain(cur);
		}

		// `cur` is now the terminal literal and the string's first byte.
		oldSuffix = cur & 0xFF;
		if (depth >= LZD_STACK_MAX) goto done;
		chainStack[depth++] = (unsigned char)oldSuffix;

		unsigned long newIndex = LZFreeIndex;

		// The assembly pushed while walking and popped while writing, so the
		// emitted order is the reverse of the collection order.
		for (unsigned long i = depth; i-- > 0; )
			*out++ = chainStack[i];

		// Add (previous code, first byte just emitted) as the next entry.
		if (newIndex < LZD_MAX_CODES)
		{
			LZH_SetSuffix(newIndex, (unsigned char)oldSuffix);
			LZH_SetChain(newIndex, (unsigned short)oldChain);
			LZFreeIndex = newIndex + 1;
		}

		oldChain = code;

		if (LZFreeIndex >= LZMaxIndex && nBits < 12)
		{
			nBits++;
			LZMaxIndex <<= 1;
			LZCodeMask   = (LZCodeMask << 1) | 1;
		}
	}

done:
	return (long)(out - dest);
}

#undef LZD_READ_CODE

#if MC2_X86_ASM_LZDECOMP
// Original x86 assembly implementation, preserved for reference only.
long LZDecomp_x86asm (MemoryPtr dest, MemoryPtr src, unsigned long srcLen)
{
	long result = 0;

	__asm
	{
		mov		esi,src
		mov		ebx,srcLen

		mov		edi,dest
		xor		eax,eax

		xor		ecx,ecx							// CH and CL used
		lea		ebx,[ebx+esi-3]

		mov		LZOldChain,eax
		mov		LZSrcBufEnd,ebx

		mov		LZChain,eax
		mov		LZMaxIndex,eax

		mov		LZCodeMask,eax
		mov		LZFreeIndex,eax
	
		mov		LZCodeMask,MAX_BIT_INDEX-1
		mov		LZMaxIndex,MAX_BIT_INDEX		//max index for 9 bits == 512

		mov		LZFreeIndex,HASH_FREE			//set index to 258
		mov		LZOldSuffix,al

		mov		ch,BASE_BITS
		jmp		GetCode

//--------------------------------------------------------------------------
//                                                                          
// ClearHash restarts decompression assuming that it is starting from the   
//           beginning                                                      
//                                                                          
//--------------------------------------------------------------------------

ClearHash:
		mov		ch,BASE_BITS		        	//set up for nine bit codes
		mov		LZCodeMask,MAX_BIT_INDEX-1

		mov		LZMaxIndex,MAX_BIT_INDEX    	//max index for 9 bits == 512
		mov		LZFreeIndex,HASH_FREE       	//set index to 258
		
		cmp		esi,LZSrcBufEnd
	   	ja		error

		mov		eax,[esi+0]
		xor		ebx,ebx

		shr		eax,cl

		add		cl,ch
		mov		edx,LZCodeMask

		mov		bl,cl
		and   	cl,07h

		shr   	ebx,3
		and		eax,edx

		add		esi,ebx
		nop

		mov		LZOldChain,eax      	//previous Chain Offset.
		mov		LZOldSuffix,al

		mov		[edi],al
		inc		edi
//-------------------------------------------------------------------------
// ReadCode gets the next hash code (9 BITS) from LZDOSBuff
//         this WILL ReadFile more data if the buffer is empty
//-------------------------------------------------------------------------
GetCode:
		cmp		esi,LZSrcBufEnd
    	ja		error						// Read Passed End?

		mov		eax,[esi+0]
		xor		ebx,ebx

		shr		eax,cl

		add		cl,ch
		mov		edx,LZCodeMask

		mov		bl,cl
		and   	cl,07h

		shr   	ebx,3
		and		eax,edx

		add		esi,ebx
		nop

		cmp		eax,HASH_EOF
		je		eof

		cmp		eax,HASH_CLEAR         		//are we to clear out hash table?
		je		ClearHash
//---------------------------------------------------------------------------
//                                                                           
// Handle Chain acts on two types of Codes, A previously tabled one and a new
// one. On a previously tabled one, the chain value and suffix for that code 
// are preserved into OldSuffix and OldChain. The block operates on searching
// backward in the chains until a chain offset of 0-255 is found (meaning the
// terminal character has been reached.) Each character in the chain is saved
// on the stack.                                                             
//                                                                           
//---------------------------------------------------------------------------

//HandleChain:
		mov		edx,esp 		
		dec		esp

		mov		LZChain,eax        				//Save new chain as well
		lea		ebx, [LZHashBuffer+eax+eax*2]
		
		cmp		eax,LZFreeIndex					//is code in HASH TABLE already?
		jl		InTable      					//if yes, then process chain
	
		mov		al,LZOldSuffix					//get back the old suffix and plant it
		mov		[esp],al                   		//onto the stack for processing later
		dec		esp
		mov		[ebx+2],al				
		mov		eax,LZOldChain     				//get Old chain for creation of Old Chain
		mov		[ebx],ax
		lea		ebx, [LZHashBuffer+eax+eax*2]

InTable:
		test	ah,ah							//(ax<255) is current chain a character?  
		jz		ChainEnd   						//if yes, then begin Print out

		mov		al,[ebx+2]		 				//push suffix onto stack
		mov		[esp],al                   		//onto the stack for processing later

		dec		esp
		movzx	eax,word ptr [ebx]				//get chain to this code
		lea		ebx, [LZHashBuffer+eax+eax*2]
		jmp   InTable            				//and keep filling up
		
ChainEnd:
		mov		LZOldSuffix,al				//save last character in chain
		mov		[esp],al                   		//onto the stack for processing later

		sub		edx,esp 	
		mov		ebx,LZFreeIndex    				//get new code number index

send_bytes:
		mov		al,[esp]
		inc		esp

		mov		[edi],al
		inc		edi

		dec		edx
		jnz		send_bytes

//---------------------------------------------------------------------------
//                                                                           
// Here we add another chain to the hash table so that we continually use it 
// for more decompressions.                                                  
//                                                                           
//---------------------------------------------------------------------------

		mov		al,LZOldSuffix
		mov		edx,LZOldChain

		mov		byte ptr [LZHashBuffer+ebx+ebx*2+2],al
		mov		word ptr [LZHashBuffer+ebx+ebx*2],dx

		inc		ebx
		mov		eax,LZChain

		mov		LZFreeIndex,ebx
		mov		edx,LZMaxIndex

		mov		LZOldChain,eax
		cmp		ebx,edx

		jl    	GetCode

		cmp		ch,12
		je		GetCode

		inc		ch
		mov		ebx,LZCodeMask

		mov		eax,LZMaxIndex
		add		ebx,ebx

		add		eax,eax
		or		ebx,1

		mov		LZMaxIndex,eax
		mov		LZCodeMask,ebx

		jmp   	GetCode


error:
eof:
		sub		edi,dest
		mov		result,edi
	}
	return(result);
}
#endif // MC2_X86_ASM_LZDECOMP