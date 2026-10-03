//---------------------------------------------------------------------------
//
// TerrTxm.cpp -- File contains class code for the Terrain Textures
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include Files
#ifndef TERRTXM_H
#include "TerrTxm.h" // PORT: match case-sensitive host path.
#endif

#ifndef MATHFUNC_H
#include "mathfunc.h"
#endif

#ifndef CIDENT_H
#include "Cident.h" // PORT: match case-sensitive host path.
#endif

#ifndef PATHS_H
#include "Paths.h" // PORT: match case-sensitive host path.
#endif

#ifndef INIFILE_H
#include "Inifile.h" // PORT: match case-sensitive host path.
#endif

#ifndef TGAINFO_H
#include "tgainfo.h"
#endif

#ifndef TERRAIN_H
#include "Terrain.h" // PORT: match case-sensitive host path.
#endif

#include "windows.h"

//---------------------------------------------------------------------------
// Static Globals
#define TILE_THRESHOLD 0

long tileCacheReqs = 0;
long tileCacheHits = 0;
long tileCacheMiss = 0;

long TERRAIN_TXM_SIZE = 64;

// PORT: M8-B12 diagnostic map. purgeTransitions deliberately discards the
// retail search table after load, but the road trace still needs to name the
// .txm behind a live texture index. Static storage does not perturb the GameOS
// heap census and is silent unless the trace asks for it.
static DWORD mc2TransitionTypeByTexture[MC_MAX_TERRAIN_TXMS];
static DWORD mc2TransitionOverlayByTexture[MC_MAX_TERRAIN_TXMS];
static bool mc2TransitionInfoValid[MC_MAX_TERRAIN_TXMS];

bool MC2_GetTransitionInfo (DWORD texture, DWORD& typeInfo, DWORD& overlayInfo)
{
	if (texture >= MC_MAX_TERRAIN_TXMS || !mc2TransitionInfoValid[texture])
		return false;
	typeInfo = mc2TransitionTypeByTexture[texture];
	overlayInfo = mc2TransitionOverlayByTexture[texture];
	return true;
}

#define MAX_MASK_NUM		14

#define USE_TRANSITIONS
#define MAX_MC2_TRANSITIONS	8192

bool TerrainTextures::terrainTexturesInstrumented = false;
long TerrainTextures::nextAvailable = 0;
long TerrainTextures::numTxms = 0;

extern bool InEditor;

static bool MC2_PixelsWouldVanish (const MemoryPtr pixels, long decoded)
{
	bool allPinkEnvelope = true;
	bool hasNonMatteContent = false;
	const unsigned char* p = (const unsigned char*)pixels;
	for (long i = 0; i < decoded; i += 4)
	{
		const unsigned char b = p[i + 0];
		const unsigned char g = p[i + 1];
		const unsigned char r = p[i + 2];
		const bool pinkEnvelope = b >= 160 && r >= 160 && g <= 96 &&
			b >= g + 80 && r >= g + 80;
		if (!pinkEnvelope)
			return false;
		if (b != 255 || g != 0 || r != 255)
			hasNonMatteContent = true;
	}
	return allPinkEnvelope && hasNonMatteContent;
}

static bool MC2_TransitionCacheWouldVanish (const char* path)
{
	File txmFile;
	if (txmFile.open(path) != NO_ERR)
		return false;
	const DWORD fileSize = txmFile.fileSize();
	MemoryPtr fileRAM = (MemoryPtr)malloc(fileSize);
	MemoryPtr pixels = (MemoryPtr)malloc(TERRAIN_TXM_SIZE * TERRAIN_TXM_SIZE * sizeof(DWORD));
	const long read = txmFile.read(fileRAM, fileSize);
	txmFile.close();
	if (read != (long)fileSize)
	{
		free(fileRAM);
		free(pixels);
		return false;
	}
	const long decoded = LZDecomp(pixels, fileRAM, fileSize);
	free(fileRAM);
	if (decoded != TERRAIN_TXM_SIZE * TERRAIN_TXM_SIZE * (long)sizeof(DWORD))
	{
		free(pixels);
		return false;
	}

	const bool wouldVanish = MC2_PixelsWouldVanish(pixels, decoded);
	free(pixels);
	return wouldVanish;
}
//---------------------------------------------------------------------------
// Class TerrainTextures
//
// At Present, this class doesn't load textures til it needs 'em.  Once loaded
// they are loaded forever.  This may not be a problem, but if it becomes one,
// the textures can be preloaded during the init.
long TerrainTextures::init (char *fileName, char *baseName)
{
	long i;
	readTransitionCacheWithSources = false;
	if (InEditor)
		quickLoad = false;
	else
	{
		quickLoad = true;

		//Check for the magic list and then for all of the textures on the magic list.
		FullPathFileName listPath;
		listPath.init(texturePath,baseName,".lst");

		File listFile;
		long result = listFile.open(listPath);
		if (result == NO_ERR)
		{
			bool malformedCache = false;
			while (!listFile.eof())
			{
				char listName[1024];
				listFile.readString((MemoryPtr)listName);
				if (!fileExists(listName))
				{
					quickLoad = false;
					break;
				}
				if (MC2_TransitionCacheWouldVanish(listName))
					malformedCache = true;
			}
			if (!quickLoad)
				malformedCache = false;
			if (malformedCache)
			{
				// Load source textures and masks, but keep reading the valid cache
				// entries. Only malformed entries fall through to the combiner.
				quickLoad = false;
				readTransitionCacheWithSources = true;
			}
		}
		else
		{
			quickLoad = false;
		}
	}
	tileCacheReqs = 0;
	tileCacheHits = 0;
	tileCacheMiss = 0;

	globalMipLevel = 0;

	FullPathFileName txmFileName;

	//--------------------------------------------------------
	// First setup the user heap with amount of RAM requested
	tileHeap = new UserHeap;
	gosASSERT(tileHeap != NULL);

	tileRAMHeap = new UserHeap;
	gosASSERT(tileRAMHeap != NULL);

	txmFileName.init(texturePath,fileName,".FIT");

	FitIniFile textureFile;

	long result = textureFile.open(txmFileName);
	gosASSERT(result == NO_ERR);

	result = textureFile.seekBlock("Main");
	gosASSERT(result == NO_ERR);

	result = textureFile.readIdLong("MaxTerrainTextures",numTxms);
	gosASSERT(result == NO_ERR);

	result = textureFile.readIdLong("MaxTerrainTypes",numTypes);
	gosASSERT(result == NO_ERR);

	result = textureFile.readIdLong("MaxTerrainOverlays",numOverlays);
	gosASSERT(result == NO_ERR);

	result = textureFile.readIdLong("MaxTerrainDetails",numDetails);
	gosASSERT(result == NO_ERR);

	//result = textureFile.readIdLong("MaxTerrainTransitions",numTransitions);
	//gosASSERT(result == NO_ERR);
	numTransitions = MAX_MC2_TRANSITIONS;

	unsigned long terrainTileCacheSize = 0;
	result = textureFile.readIdULong("TextureMemSize",terrainTileCacheSize);
	gosASSERT(result == NO_ERR);

	result = tileHeap->init(65536,"TextureRAM");
	gosASSERT(result == NO_ERR);

	result = tileRAMHeap->init(terrainTileCacheSize,"TextureRAM");
	gosASSERT(result == NO_ERR);

	localBaseName = (char *)tileRAMHeap->Malloc(strlen(baseName)+1);
	strcpy(localBaseName,baseName);

	//--------------------------------------------------------
	// Create the lists of pointers to the textures.
	textures = (TerrainTXMPtr)tileHeap->Malloc(numTxms * sizeof(TerrainTXM));
	gosASSERT(textures != NULL);

	memset(textures,-1,numTxms * sizeof(TerrainTXM));

	types = (MC_TerrainTypePtr)tileRAMHeap->Malloc(numTypes * sizeof(MC_TerrainType));
	gosASSERT(types != NULL);

	memset(types,0,numTypes * sizeof(MC_TerrainType));

	overlays = (MC_OverlayTypePtr)tileRAMHeap->Malloc(numOverlays * sizeof(MC_OverlayType));
	gosASSERT(overlays != NULL);

	memset(overlays, 0, numOverlays * sizeof(MC_OverlayType));

	details = (MC_DetailTypePtr)tileHeap->Malloc(numDetails * sizeof(MC_DetailType));
	gosASSERT(details != NULL);

	memset(details, 0, numDetails * sizeof(MC_DetailType));

	transitions = (TransitionTypePtr)tileRAMHeap->Malloc(numTransitions * sizeof(TransitionType));
	gosASSERT(transitions != NULL);

	memset(transitions, -1, numTransitions * sizeof(TransitionType));

	//-----------------------------------
	// Read in the Base Terrain Textures.
	for (i=0;i<numTypes;i++)
	{
		char blockName[512];
		sprintf(blockName,"TerrainType%d",i);

		result = textureFile.seekBlock(blockName);
		if (result == NO_ERR)
		{
			result = textureFile.readIdLong("TerrainId",types[i].terrainId);
			gosASSERT(result == NO_ERR);
	
			char tmpy[2048];
			result = textureFile.readIdString("TerrainName",tmpy,2047);
			gosASSERT(result == NO_ERR);
			types[i].terrainName = (char *)tileRAMHeap->Malloc(strlen(tmpy)+1);
			gosASSERT(types[i].terrainName != NULL);
			strcpy(types[i].terrainName,tmpy);
	
			result = textureFile.readIdString("MaskName",tmpy,2047);
			gosASSERT(result == NO_ERR);
			types[i].maskName = (char *)tileRAMHeap->Malloc(strlen(tmpy)+1);
			gosASSERT(types[i].maskName != NULL);
			strcpy(types[i].maskName,tmpy);
	
			result = textureFile.readIdLong("TerrainPriority",types[i].terrainPriority);
			gosASSERT(result == NO_ERR);
	
			result = textureFile.readIdULong("TerrainTacRGB",types[i].terrainMapRGB);
			gosASSERT(result == NO_ERR);

			result = textureFile.readIdLong( "NameID", types[i].nameId );
			gosASSERT( result == NO_ERR );
	
			types[i].textureData = NULL;
			types[i].maskData = NULL;
		}
		else			//MUST load up blank ones or overlays will get trashed whenever we add a terrain type!!
		{
			types[i].terrainId = types[0].terrainId;
			types[i].terrainName = NULL;
			types[i].maskName = NULL;
			types[i].terrainPriority = types[0].terrainPriority;
			types[i].terrainMapRGB = types[0].terrainMapRGB;
			types[i].textureData = NULL;
			types[i].maskData = NULL;
		}
	}

	//-------------------------------------------------------------
	// Now preload the masks and base texture data
	// Assume as per art lead on 4/1/99 only one txm and 14 masks.
	for (i=0;i<numTypes;i++)
	{
		if ((i && (types[i].terrainId != types[0].terrainId)) || !i)
		{
			types[i].textureData = (MemoryPtr *)tileRAMHeap->Malloc(sizeof(MemoryPtr) * MC_MAX_MIP_LEVELS);
			gosASSERT(types[i].textureData != NULL);
			
			memset(types[i].textureData,0,sizeof(MemoryPtr) * MC_MAX_MIP_LEVELS);

			types[i].maskData = (MemoryPtr *)tileRAMHeap->Malloc(sizeof(MemoryPtr) * MC_MASK_NUM * MC_MAX_MIP_LEVELS);
			gosASSERT(types[i].maskData != NULL);
			
			memset(types[i].maskData,0,sizeof(MemoryPtr) * MC_MASK_NUM);

			long terrainTextureIndex = initTexture(i);
			types[i].baseTXMIndex = terrainTextureIndex;

			initMask(i);
		}
		else if (i && (types[i].terrainId == types[0].terrainId))
		{
			MemoryPtr ourRAM = (MemoryPtr)tileRAMHeap->Malloc(16 * 16 * sizeof(DWORD));
			gosASSERT(ourRAM != NULL);
			
			memset(ourRAM,0,16 * 16 * sizeof(DWORD));
			
			textures[nextAvailable].mcTextureNodeIndex = mcTextureManager->textureFromMemory((DWORD *)ourRAM,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink,16);
			nextAvailable++;

			if (nextAvailable > numTxms)
				STOP(("Too Many Terrain Textures!  Its at %d right now!",numTxms));

			types[i].baseTXMIndex = nextAvailable-1;
		}
	}

	//------------------------------------------
	// Read in the Overlay Textures and Shapes.
	for (i=0;i<numOverlays;i++)
	{
		char blockName[512];
		sprintf(blockName,"OverlayType%d",i);

		result = textureFile.seekBlock(blockName);
		gosASSERT(result == NO_ERR);

		result = textureFile.readIdLong("OverlayId",overlays[i].overlayId);
		gosASSERT(result == NO_ERR);

		char tmpy[2048];
		result = textureFile.readIdString("OverlayName",tmpy,2047);
		gosASSERT(result == NO_ERR);
		overlays[i].overlayName = (char *)tileRAMHeap->Malloc(strlen(tmpy)+1);
		gosASSERT(overlays[i].overlayName != NULL);
		strcpy(overlays[i].overlayName,tmpy);

		result = textureFile.readIdLong("NumOverlays",overlays[i].numTextures);
		gosASSERT(result == NO_ERR);

		result = textureFile.readIdLong("OldOverlayNum",overlays[i].oldOverlayId);
		gosASSERT(result == NO_ERR);

		result = textureFile.readIdBoolean("IsMLR",overlays[i].isMLRAppearance);
		gosASSERT(result == NO_ERR);

   		result = textureFile.readIdULong("TerrainTacRGB",overlays[i].terrainMapRGB);
   		gosASSERT(result == NO_ERR);
		
 		overlays[i].overlayData = NULL;		//Contains MLR shape or TerrainTXM Data
	}

	//--------------------------------------------------------------
	// Now preload the overlay texture data 
	for (i=0;i<numOverlays;i++)
	{
		overlays[i].overlayData = (MemoryPtr *)tileRAMHeap->Malloc(sizeof(MemoryPtr) * overlays[i].numTextures * MC_MAX_MIP_LEVELS);
		gosASSERT(overlays[i].overlayData != NULL);
		
		memset(overlays[i].overlayData,0,sizeof(MemoryPtr) * overlays[i].numTextures * MC_MAX_MIP_LEVELS);

		for (long j=0;j<overlays[i].numTextures;j++)
		{
			long overlayTextureIndex = initOverlay(i,j,overlays[i].overlayName);
			if (!j)
				overlays[i].baseTXMIndex = overlayTextureIndex;
				
			if (!i && !j)
				firstOverlay = overlayTextureIndex;
		}
	}

	//------------------------------------------
	// Read in the Detail Textures
	for (i=0;i<numDetails;i++)
	{
		char blockName[512];
		sprintf(blockName,"DetailType%d",i);

		result = textureFile.seekBlock(blockName);
		gosASSERT(result == NO_ERR);

		result = textureFile.readIdLong("DetailId",details[i].detailId);
		gosASSERT(result == NO_ERR);

		char tmpy[2048];
		result = textureFile.readIdString("DetailName",tmpy,2047);
		gosASSERT(result == NO_ERR);
		details[i].detailName = (char *)tileRAMHeap->Malloc(strlen(tmpy)+1);
		gosASSERT(details[i].detailName != NULL);
		strcpy(details[i].detailName,tmpy);

		result = textureFile.readIdLong("NumDetails",details[i].numDetails);
		gosASSERT(result == NO_ERR);

		result = textureFile.readIdFloat("FrameRate",details[i].frameRate);
		gosASSERT(result == NO_ERR);
		
		result = textureFile.readIdFloat("TilingFactor",details[i].tilingFactor);
		gosASSERT(result == NO_ERR);
		
 		details[i].detailData = NULL;
	}

	//--------------------------------------------------------------
	// Now preload the detail texture data 
	for (i=0;i<numDetails;i++)
	{
		details[i].detailData = (MemoryPtr *)tileRAMHeap->Malloc(sizeof(MemoryPtr) * details[i].numDetails * MC_MAX_MIP_LEVELS);
		gosASSERT(details[i].detailData != NULL);
		
		memset(details[i].detailData,0,sizeof(MemoryPtr) * details[i].numDetails * MC_MAX_MIP_LEVELS);

		for (long j=0;j < details[i].numDetails; j++)
		{
			long detailTextureIndex = initDetail(i,j);
			if (j==0)
				details[i].baseTXMIndex = detailTextureIndex;
		}
	}


	return(NO_ERR);
}

void TerrainTextures::initializeStatistics()
{
	
	if (!terrainTexturesInstrumented)
	{
			StatisticFormat( "" );
			StatisticFormat( "MechCommander 2 Terrain Texture" );
			StatisticFormat( "===============================" );
			StatisticFormat( "" );

			AddStatistic("Total Textures","Textures",gos_DWORD, (void*)&(numTxms), Stat_Total);

			AddStatistic("Textures Used ","Textures",gos_DWORD, (void*)&(nextAvailable), Stat_Total);

			StatisticFormat( "" );
			StatisticFormat( "" );

			terrainTexturesInstrumented = true;
	}
}
//---------------------------------------------------------------------------
long TerrainTextures::textureFromMemoryAlpha (MemoryPtr ourRAM, long mipLevel)
{
	textures[nextAvailable].mcTextureNodeIndex = mcTextureManager->textureFromMemory((DWORD *)ourRAM,gos_Texture_Keyed,gosHint_DisableMipmap | gosHint_DontShrink,mipLevel);

	nextAvailable++;

	if (nextAvailable > numTxms)
		STOP(("Too Many Terrain Textures!  Its at %d right now!",numTxms));

	return(nextAvailable-1);	
}

//---------------------------------------------------------------------------
long TerrainTextures::textureFromMemory (MemoryPtr ourRAM, long mipLevel)
{
	textures[nextAvailable].mcTextureNodeIndex = mcTextureManager->textureFromMemory((DWORD *)ourRAM,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink,mipLevel);

	nextAvailable++;

	if (nextAvailable > numTxms)
		STOP(("Too Many Terrain Textures!  Its at %d right now!",numTxms));

	return(nextAvailable-1);	
}
	
//---------------------------------------------------------------------------
long TerrainTextures::loadTextureMemory (char *textureName, long mipSize)
{
	textures[nextAvailable].mcTextureNodeIndex = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);

	nextAvailable++;

	if (nextAvailable > numTxms)
		STOP(("Too Many Terrain Textures!  Its at %d right now!",numTxms));

	return(nextAvailable-1);
}	

//---------------------------------------------------------------------------
long TerrainTextures::loadDetailMemory (char *textureName, long mipSize)
{
	textures[nextAvailable].mcTextureNodeIndex = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DontShrink);

	nextAvailable++;

	if (nextAvailable > numTxms)
		STOP(("Too Many Terrain Textures!  Its at %d right now!",numTxms));

	return(nextAvailable-1);
}	

//---------------------------------------------------------------------------
long TerrainTextures::loadOverlayMemory (char *textureName)
{
	textures[nextAvailable].mcTextureNodeIndex = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);

	nextAvailable++;

	if (nextAvailable > numTxms)
		STOP(("Too Many Terrain Textures!  Its at %d right now!",numTxms));

	return(nextAvailable-1);
}	

//---------------------------------------------------------------------------
long TerrainTextures::initDetail (long typeNum, long detailNum)
{
	//-----------------------------------------------------------------------
	// This function loads the TGA pointed to by name and number into sysRAM
	// It then copies the raw TGA texture to GOS Texture Memory.
	// We now control the MIP levels.  Load all MIP levels here!
	long txmResult = -1;
	for (long j=0;j<MC_MAX_MIP_LEVELS;j++)
	{
		//---------------------------------------------
		// Change Texture Path to reflect texture size
		char mipPath[512];
		long mipSize = 0;
		switch (j)
		{
			case 0:
				sprintf(mipPath,"%s%d\\",texturePath,TERRAIN_TXM_SIZE);
				mipSize = TERRAIN_TXM_SIZE;
				break;	

			case 1:
				sprintf(mipPath,"%s%d\\",texturePath,(TERRAIN_TXM_SIZE>>1));
				mipSize = (TERRAIN_TXM_SIZE>>1);
				break;

			case 2:
				sprintf(mipPath,"%s%d\\",texturePath,(TERRAIN_TXM_SIZE>>2));
				mipSize = (TERRAIN_TXM_SIZE>>2);
				break;

			case 3:
				sprintf(mipPath,"%s%d\\",texturePath,(TERRAIN_TXM_SIZE>>3));
				mipSize = (TERRAIN_TXM_SIZE>>3);
				break;
		}

		char tmpy[512];
		sprintf(tmpy,"%s%04d",details[typeNum].detailName,detailNum);

		FullPathFileName fileName;
		fileName.init(mipPath,tmpy,".tga");

		long result = loadDetailMemory(fileName,mipSize);
		if (j==0)
			txmResult = result;
	}

	return(txmResult);
}	

//---------------------------------------------------------------------------
long TerrainTextures::initTexture (long typeNum)
{
	//-----------------------------------------------------------------------
	// This function loads the TGA pointed to by name and number into sysRAM
	// It then copies the raw TGA texture to GOS Texture Memory.
	// We now control the MIP levels.  Load all MIP levels here!
	long txmResult = -1;
	for (long j=0;j<MC_MAX_MIP_LEVELS;j++)
	{
		//---------------------------------------------
		// Change Texture Path to reflect texture size
		char mipPath[512];
		long mipSize = 0;
		switch (j)
		{
			case 0:
				sprintf(mipPath,"%s%d\\",texturePath,TERRAIN_TXM_SIZE);
				mipSize = TERRAIN_TXM_SIZE;
				break;	

			case 1:
				sprintf(mipPath,"%s%d\\",texturePath,(TERRAIN_TXM_SIZE>>1));
				mipSize = (TERRAIN_TXM_SIZE>>1);
				break;

			case 2:
				sprintf(mipPath,"%s%d\\",texturePath,(TERRAIN_TXM_SIZE>>2));
				mipSize = (TERRAIN_TXM_SIZE>>2);
				break;

			case 3:
				sprintf(mipPath,"%s%d\\",texturePath,(TERRAIN_TXM_SIZE>>3));
				mipSize = (TERRAIN_TXM_SIZE>>3);
				break;
		}

		FullPathFileName fileName;
		fileName.init(mipPath,types[typeNum].terrainName,".tga");

		long result = 0;
		if (InEditor || !quickLoad)
		{
			File tgaFile;
			result = tgaFile.open(fileName);
			gosASSERT(result == NO_ERR);
	
			struct TGAFileHeader header;
	
			tgaFile.read((MemoryPtr)&header,sizeof(TGAFileHeader));
	
			if (mipSize != header.width)
				mipSize = header.width;
	
			tgaFile.seek(0);
	
			MemoryPtr ourRAM = (MemoryPtr)tileRAMHeap->Malloc(mipSize * mipSize * sizeof(DWORD));
			gosASSERT(ourRAM != NULL);
	
			loadTGATexture(&tgaFile,ourRAM,mipSize,mipSize);
	
			types[typeNum].textureData[j] = ourRAM;
	
			tgaFile.close();
		}

		result = loadTextureMemory(fileName,mipSize);
		if (j==0)
			txmResult = result;
	}

	return(txmResult);
}	

//---------------------------------------------------------------------------
void TerrainTextures::initMask (long typeNum)
{
	long i;
	if (quickLoad)
		return;

	//----------------------------------------------------------------------------
	// The masks are stored in sysRAM only.  Need to load MC_MASK_NUM * miplevels
	// per terrain type.
	for (i=0;i<MC_MAX_MIP_LEVELS;i++)
	{
		//---------------------------------------------
		// Change Texture Path to reflect texture size
		char mipPath[512];
		long mipSize = 0;
		switch (i)
		{
			case 0:
				sprintf(mipPath,"%s%dmask\\",texturePath,(TERRAIN_TXM_SIZE>>0));
				mipSize = (TERRAIN_TXM_SIZE>>0);
				break;	

			case 1:
				sprintf(mipPath,"%s%dmask\\",texturePath,(TERRAIN_TXM_SIZE>>1));
				mipSize = (TERRAIN_TXM_SIZE>>1);
				break;

			case 2:
				sprintf(mipPath,"%s%dmask\\",texturePath,(TERRAIN_TXM_SIZE>>2));
				mipSize = (TERRAIN_TXM_SIZE>>2);
				break;

			case 3:
				sprintf(mipPath,"%s%dmask\\",texturePath,(TERRAIN_TXM_SIZE>>3));
				mipSize = (TERRAIN_TXM_SIZE>>3);
				break;
		}

		for (long j=0;j<MC_MASK_NUM;j++)
		{
			char tmpy[512];
			sprintf(tmpy,"%s%04d",types[typeNum].maskName,j);

			FullPathFileName fileName;
			fileName.init(mipPath,tmpy,".tga");

			File tgaFile;

#ifdef _DEBUG
			long result =
#endif
			tgaFile.open(fileName);
			gosASSERT(result == NO_ERR);

			MemoryPtr ourRAM = (MemoryPtr)tileRAMHeap->Malloc(mipSize * mipSize * sizeof(BYTE));
			gosASSERT(ourRAM != NULL);

			loadTGAMask(&tgaFile,ourRAM,mipSize,mipSize);

			types[typeNum].maskData[j+(i*MC_MASK_NUM)] = ourRAM;

			tgaFile.close();
		}
	}
}	

//---------------------------------------------------------------------------
long TerrainTextures::initOverlay (long overlayNum, long txmNum, char *txmName)
{
	long txmResult = -1;
	for (long j=0;j<MC_MAX_MIP_LEVELS;j++)
	{
		//---------------------------------------------
		// Change Texture Path to reflect texture size
		char mipPath[512];
		long mipSize = 0;
		switch (j)
		{
			case 0:
				sprintf(mipPath,"%s%dOverlays\\",texturePath,TERRAIN_TXM_SIZE);
				mipSize = TERRAIN_TXM_SIZE;
				break;	

			case 1:
				sprintf(mipPath,"%s%dOverlays\\",texturePath,(TERRAIN_TXM_SIZE>>1));
				mipSize = (TERRAIN_TXM_SIZE>>1);
				break;

			case 2:
				sprintf(mipPath,"%s%dOverlays\\",texturePath,(TERRAIN_TXM_SIZE>>2));
				mipSize = (TERRAIN_TXM_SIZE>>2);
				break;

			case 3:
				sprintf(mipPath,"%s%dOverlays\\",texturePath,(TERRAIN_TXM_SIZE>>3));
				mipSize = (TERRAIN_TXM_SIZE>>3);
				break;
		}

		char tmpy[512];
		sprintf(tmpy,"%s%04d",txmName,txmNum);

		FullPathFileName fileName;
		fileName.init(mipPath,tmpy,".tga");

		long result = 0;
		//------------------------------------------------------------
		if (InEditor || !quickLoad)
		{
			File tgaFile;

			result = tgaFile.open(fileName);
			gosASSERT(result == NO_ERR);

			MemoryPtr ourRAM = (MemoryPtr)tileRAMHeap->Malloc(mipSize * mipSize * sizeof(DWORD));
			gosASSERT(ourRAM != NULL);

			loadTGATexture(&tgaFile,ourRAM,mipSize,mipSize);

			overlays[overlayNum].overlayData[txmNum] = ourRAM;

			tgaFile.close();
		}
		//------------------------------------------------------------
		result = loadOverlayMemory(fileName);

		if (j==0)
			txmResult = result;
	}

	return(txmResult);
}	

//---------------------------------------------------------------------------
long TerrainTextures::setOverlay (DWORD overlayInfo)
{
	long i;
	//-------------------------------------------------------------
	// This still uses the old Overlay Indicies from MC1 and MCX!
	long txmHandle = 0xffff0000;
	for (i=0;i<numOverlays;i++)
	{
		if ((overlayInfo >= overlays[i].oldOverlayId) && 
			(overlayInfo <= (overlays[i].oldOverlayId+18)))
		{
			int index = (overlayInfo - overlays[i].oldOverlayId);
			if ( overlays[i].numTextures> 9 ) // roads have new indices
			{
				switch( index )
				{
				case 0:
				case 1:
				case 2:
					index = 0;
					break;

				case 3:
				case 4:
				case 5:
					index = 1;
					break;

				case 6:
				case 7:
				case 8:
				case 9:
				case 10:
				case 11:
				case 12:
				case 13:
				case 14:
					index = index - 4;
					break;

				case 15:
					 index = 9;
					 break;
				case 17:
					index = 7;
					break;
				case 18:
				case 16:
					index = index - 8;
					break;

				}
			}

			txmHandle = overlays[i].baseTXMIndex + ( index * MC_MAX_MIP_LEVELS);
			txmHandle <<= 16;
		}
	}

	return(txmHandle);
}

//---------------------------------------------------------------------------
// this doesn't use the old indices. Those shouldn't be necessary.
long TerrainTextures::getOverlayHandle( Overlays id, int Offset )
{
	long txmHandle = 0xffff0000;
	gosASSERT( id > -2 && id < numOverlays );
	
	if ( id > -1 && id < numOverlays )
	{
		txmHandle = overlays[id].baseTXMIndex + (Offset * MC_MAX_MIP_LEVELS );
		txmHandle <<= 16;
	}
	
	return(txmHandle);

}

void TerrainTextures::getOverlayInfoFromHandle( long handle, Overlays& id, unsigned long& Offset )
{
	int i;
	id = INVALID_OVERLAY;
	Offset = -1;

	for ( i = 0; i < numOverlays; ++i )
	{
		if ( (handle >> 16) >= overlays[i].baseTXMIndex 
			 && (handle >> 16) < overlays[i].baseTXMIndex + overlays[i].numTextures * MC_MAX_MIP_LEVELS )
		{
			id = (Overlays)i;
			long tmp = (handle >> 16) - overlays[i].baseTXMIndex;
			Offset = tmp/MC_MAX_MIP_LEVELS;			
		}
	}
}

//---------------------------------------------------------------------------
void TerrainTextures::combineTxm (MemoryPtr dest, DWORD binNumber, long type, long mipLevel)
{
	long i;
	MemoryPtr combineRAM = types[type].textureData[mipLevel];
	MemoryPtr maskRAM = types[type].maskData[(binNumber-1) + (mipLevel * MC_MASK_NUM)];

	long mipSize = 0;
	switch (mipLevel)
	{
		case 0:
			mipSize = (TERRAIN_TXM_SIZE>>0);
			break;	

		case 1:
			mipSize = (TERRAIN_TXM_SIZE>>1);
			break;

		case 2:
			mipSize = (TERRAIN_TXM_SIZE>>2);
			break;

		case 3:
			mipSize = (TERRAIN_TXM_SIZE>>3);
			break;
	}

	for (i=0;i<(mipSize * mipSize);i++)
	{
		if (maskRAM[i])
		{
			float combPercent = float(maskRAM[i]) / 255.0f;
			float destPercent = 1.0f - combPercent;

			if (combPercent == 1.0f)
			{
				*dest = *combineRAM;
				dest++;
				combineRAM++;

				*dest = *combineRAM;
				dest++;
				combineRAM++;

				*dest = *combineRAM;
				dest++;
				combineRAM++;

				if (*dest < maskRAM[i])
					*dest = maskRAM[i];
				dest++;
				combineRAM++;
			}
			else
			{
				float combValue = combPercent * float(*combineRAM);
				float destValue = destPercent * float(*dest);
				combValue += destValue;
				unsigned char value = (unsigned char)combValue;
				*dest = value;
				dest++;
				combineRAM++;

				combValue = combPercent * float(*combineRAM);
				destValue = destPercent * float(*dest);
				combValue += destValue;
				value = (unsigned char)combValue;
				*dest = value;
				dest++;
				combineRAM++;


				combValue = combPercent * float(*combineRAM);
				destValue = destPercent * float(*dest);
				combValue += destValue;
				value = (unsigned char)combValue;
				*dest = value;
				dest++;
				combineRAM++;

				//This is the alpha channel.  DO NOT COMBINE!!!
				// Just copy in the CombineTXM AlphA!!
				//combValue = combPercent * float(*combineRAM);
				//destValue = destPercent * float(*dest);
				//combValue += destValue;
				//value = (unsigned char)combValue;
				if (*dest < maskRAM[i])
					*dest = maskRAM[i];
				dest++;
				combineRAM++;
			}
		}
		else
		{
			dest += 4;
			combineRAM += 4;
		}
	}
}	

//---------------------------------------------------------------------------
void forceAlphaOpaque(MemoryPtr dest, long mipLevel)
{
	long i;
	long mipSize = 0;
	switch (mipLevel)
	{
		case 0:
			mipSize = (TERRAIN_TXM_SIZE>>0);
			break;	

		case 1:
			mipSize = (TERRAIN_TXM_SIZE>>1);
			break;

		case 2:
			mipSize = (TERRAIN_TXM_SIZE>>2);
			break;

		case 3:
			mipSize = (TERRAIN_TXM_SIZE>>3);
			break;
	}

	for (i=0;i<(mipSize * mipSize);i++)
	{
		dest++;
		dest++;
		dest++;
		*dest = 0xff;
		dest++;
	}
}

//---------------------------------------------------------------------------
// M8-B12, 2026-08-20. The composed road tiles differ between missions in a way
// that is uniform and therefore not random: every composed tile on mc2_01 tops
// out at alpha 255 with 8 distinct alpha values, and every one on mc2_08 tops
// out at exactly 140 with 125 of them. 140 is too specific to be coincidence.
//
// This reports the OVERLAY SOURCE as the combiner sees it -- its alpha range
// and how many distinct alpha values it carries -- per overlay type, once each.
// If the source already tops out at 140, the ramp is in the art or in how it
// was decoded, and the combiner is innocent. If the source reaches 255 and the
// composite does not, the loss happens inside the blend below.
//
// Those are different faults. Nothing here decides between them.
// Armed with MC2_ROAD_CENSUS=1, the same switch as the quad-side counters.
static bool MC2_RoadCensusOn2 (void)
{
	static const char* env = getenv("MC2_ROAD_CENSUS");
	static const bool on = env && *env && strcmp(env, "0") != 0;
	return on;
}

static void MC2_ReportOverlaySource (long oType, long oIndx, MemoryPtr src, long pixels)
{
	if (!MC2_RoadCensusOn2() || !src || pixels <= 0)
		return;

	// Once per (type,index): the combiner runs this thousands of times.
	static long seen[64][32];
	static bool cleared = false;
	if (!cleared) { memset(seen, 0, sizeof(seen)); cleared = true; }
	if (oType < 0 || oType >= 64 || oIndx < 0 || oIndx >= 32)
		return;
	if (seen[oType][oIndx])
		return;
	seen[oType][oIndx] = 1;

	int minA = 255, maxA = 0, distinct = 0, atFull = 0;
	bool present[256];
	memset(present, 0, sizeof(present));
	for (long i = 0; i < pixels; i++)
	{
		const unsigned char a = *(src + (i * 4) + 3);
		if (a < minA) minA = a;
		if (a > maxA) maxA = a;
		if (a == 255) atFull++;
		if (!present[a]) { present[a] = true; distinct++; }
	}

	fprintf(stderr,
		"[road-src] overlayType=%ld index=%ld pixels=%ld alpha %d..%d distinct=%d atFull=%d\n",
		oType, oIndx, pixels, minA, maxA, distinct, atFull);
	fflush(stderr);
}

void TerrainTextures::combineOverlayTxm (MemoryPtr dest, long type, long mipLevel)
{
	long i;
	//Convert overlay Type to actual overlay Index.
	long oType = 0;
	long oIndx = 0;
	for (i=0;i<numOverlays;i++)
	{
		if ((type >= overlays[i].baseTXMIndex) &&
			(type < (overlays[i].baseTXMIndex + overlays[i].numTextures)))
		{
			oIndx = type - overlays[i].baseTXMIndex;
			oType = i;
			break;
		}
	}

	MemoryPtr combineRAM = overlays[oType].overlayData[oIndx];
	gosASSERT(combineRAM != NULL);

	long mipSize = 0;
	switch (mipLevel)
	{
		case 0:
			mipSize = (TERRAIN_TXM_SIZE>>0);
			break;	

		case 1:
			mipSize = (TERRAIN_TXM_SIZE>>1);
			break;

		case 2:
			mipSize = (TERRAIN_TXM_SIZE>>2);
			break;

		case 3:
			mipSize = (TERRAIN_TXM_SIZE>>3);
			break;
	}

	MC2_ReportOverlaySource(oType, oIndx, combineRAM, mipSize * mipSize);

	for (i=0;i<(mipSize * mipSize);i++)
	{
		float combPercent = float(*(combineRAM+3)) / 255.0f;
		float destPercent = 1.0f - combPercent;

		if (combPercent != 0.0)
		{
			if (combPercent == 1.0f)
			{
				*dest = *combineRAM;
				dest++;
				combineRAM++;
	
				*dest = *combineRAM;
				dest++;
				combineRAM++;
	
				*dest = *combineRAM;
				dest++;
				combineRAM++;
	
				if (*dest < *combineRAM)
					*dest = *combineRAM;
				dest++;
				combineRAM++;
			}
			else
			{
				float combValue = combPercent * float(*combineRAM);
				float destValue = destPercent * float(*dest);
				combValue += destValue;
				unsigned char value = (unsigned char)combValue;
				*dest = value;
				dest++;
				combineRAM++;
	
				combValue = combPercent * float(*combineRAM);
				destValue = destPercent * float(*dest);
				combValue += destValue;
				value = (unsigned char)combValue;
				*dest = value;
				dest++;
				combineRAM++;
	
	
				combValue = combPercent * float(*combineRAM);
				destValue = destPercent * float(*dest);
				combValue += destValue;
				value = (unsigned char)combValue;
				*dest = value;
				dest++;
				combineRAM++;
	
				//combValue = combPercent * float(*combineRAM);
				//destValue = destPercent * float(*dest);
				//combValue += destValue;
				//value = (unsigned char)combValue;
				if (*dest < *combineRAM)
					*dest = *combineRAM;
				dest++;
				combineRAM++;
			}
		}
		else
		{
			dest += 4;
			combineRAM += 4;
		}
	}
}	

//---------------------------------------------------------------------------
void TerrainTextures::purgeTransitions (void)
{
	long i;
	for (i=0;i<nextTransition;i++)
	{
		mcTextureManager->removeTextureNode(textures[transitions[i].baseTXMIndex].mcTextureNodeIndex);
	}
	
	nextTransition = 0;
	nextAvailable = firstTransition;
}

//---------------------------------------------------------------------------
inline bool isCementType (DWORD type)
{
	bool isCement = ((type == BASE_CEMENT_TYPE) ||
					((type >= START_CEMENT_TYPE) && (type <= END_CEMENT_TYPE)));
	return isCement;
}

//---------------------------------------------------------------------------
long TerrainTextures::createTransition (DWORD typeInfo, DWORD overlayInfo)
{
	long i;
	long result = -1;
	long txmResult = 0;

	bool malformedCachedTransition = false;
	if (nextTransition < MAX_MC2_TRANSITIONS)
	{
		//------------------------------------------------------
		// Search Existing Transitions for this typeInfo.
		// Does not need to be fast yet.  May not ever since we
		// only do this on load can be done on Heidi's end.
		for (i=0;i<nextTransition;i++)
		{
			if ((typeInfo == transitions[i].transitionIndex) && 
				(overlayInfo == transitions[i].overlayIndex))
			{
				return(transitions[i].baseTXMIndex);
			}
		}

		//---------------------------------------------
		// TESTING......
		// We may have already saved out a texture which
		// is the combined version.  Check for it.  Load it, if its there and move on.
		//-----------------------------------------------------------
		// TEST.....
		// What if we saved off all of the textures instead of combining
		// every time we load?  Should lower load time.  Lets see how big
		// for all of the textures...
		// About 600K for mc2_01.  With redundancies, assume about 600K per...
		// About 20 Mb larger build.
		if (quickLoad || readTransitionCacheWithSources)
		{
			char nameData[256];
			sprintf(nameData,"%08d.%08d.txm",typeInfo,overlayInfo);

			FullPathFileName testPath;
			testPath.init(texturePath,nameData,"");

			File txmFile;
			long fr = txmFile.open(testPath);
			if (MC2_RoadCensusOn2())
			{
				static long asked = 0, missing = 0, reported = 0;
				asked++;
				if (fr != NO_ERR)
				{
					missing++;
					if (reported < 12)
					{
						reported++;
						fprintf(stderr, "[road-txm] MISSING %s\n", nameData);
					}
				}
				fprintf(stderr, "[road-txm] asked=%ld missing=%ld\r", asked, missing);
				fflush(stderr);
			}
			if (fr == NO_ERR)
			{
				long mipSize = TERRAIN_TXM_SIZE;
				DWORD fileSize = txmFile.fileSize();
				MemoryPtr fileRAM = (MemoryPtr)malloc(fileSize);
				long result = txmFile.read(fileRAM,fileSize);
				if (result != fileSize)
					STOP(("Read Error with Texture %s",(char*)testPath));

				MemoryPtr lzBuffer = (MemoryPtr)malloc(mipSize * mipSize * sizeof(DWORD));
				long bufferSize = LZDecomp(lzBuffer,fileRAM,fileSize);
				if (bufferSize != (mipSize * mipSize * sizeof(DWORD)))
					STOP(("Texture not correct size!"));

				txmFile.close();
				if (MC2_PixelsWouldVanish(lzBuffer, bufferSize))
				{
					malformedCachedTransition = true;
					free(fileRAM);
					free(lzBuffer);
				}
				else
				{

					result = textureFromMemoryAlpha(lzBuffer,mipSize);
					txmResult = result;

					free(fileRAM);
					free(lzBuffer);

					transitions[nextTransition].transitionIndex = typeInfo;
					transitions[nextTransition].baseTXMIndex = txmResult;
					transitions[nextTransition].overlayIndex = overlayInfo;
					mc2TransitionTypeByTexture[txmResult] = typeInfo;
					mc2TransitionOverlayByTexture[txmResult] = overlayInfo;
					mc2TransitionInfoValid[txmResult] = true;
					nextTransition++;

					return (txmResult);
				}
			}
		}

		if (!listTransitionFile && !malformedCachedTransition)
		{
			//Check for the magic list and then for all of the textures on the magic list.
			CreateDirectory(texturePath,NULL);

			FullPathFileName listPath;
			listPath.init(texturePath,localBaseName,".lst");

			listTransitionFile = new File;
			listTransitionFile->create(listPath);
		}

		//---------------------------------
		// No transition yet.  Make it go!
		for (long kmp=0;kmp<MC_MAX_MIP_LEVELS;kmp++)
		{
			//---------------------------------------------
			// Change Texture Path to reflect texture size
			char mipPath[512];
			long mipSize = 0;
			switch (kmp)
			{
				case 0:
					sprintf(mipPath,"%s%d\\",texturePath,(TERRAIN_TXM_SIZE>>0));
					mipSize = (TERRAIN_TXM_SIZE>>0);
					break;	

				case 1:
					sprintf(mipPath,"%s%d\\",texturePath,(TERRAIN_TXM_SIZE>>1));
					mipSize = (TERRAIN_TXM_SIZE>>1);
					break;

				case 2:
					sprintf(mipPath,"%s%d\\",texturePath,(TERRAIN_TXM_SIZE>>2));
					mipSize = (TERRAIN_TXM_SIZE>>2);
					break;


				case 3:
					sprintf(mipPath,"%s%d\\",texturePath,(TERRAIN_TXM_SIZE>>3));
					mipSize = (TERRAIN_TXM_SIZE>>3);
					break;
			}

			//-------------------------------------------------------
			// This one does not exist.  Make it.
			MemoryPtr ourRAM = (MemoryPtr)tileRAMHeap->Malloc(mipSize * mipSize * sizeof(DWORD));

			BYTE v0Type = typeInfo & 0x000000ff;
			BYTE v1Type = typeInfo >> 8;
			BYTE v2Type = typeInfo >> 16;
			BYTE v3Type = typeInfo >> 24;

			BYTE priTypes[4];
			priTypes[0] = typeInfo & 0x000000ff;
			priTypes[1] = typeInfo >> 8;
			priTypes[2] = typeInfo >> 16;
			priTypes[3] = typeInfo >> 24;

			//------------------------------------------
			// Sort types by priority.
			for (i=0;i<4;i++)
			{
				for (long j=i;j<4;j++)
				{
					if (types[priTypes[j]].terrainPriority < types[priTypes[i]].terrainPriority)
					{
						BYTE t = priTypes[i];
						priTypes[i] = priTypes[j];
						priTypes[j] = t;
					}
				}
			}

			//-----------------------------------------------
			// Draw by Priority and Mask.
			// Priority 0 is ALWAYS just copy that texture.
			// Use a random one if possible.
			MemoryPtr texture = types[priTypes[0]].textureData[kmp];
			if (!Terrain::terrainTextures2 || isCementType(priTypes[0]))
			{
				//Base is now a BLANK ALPHA MASK for new Terrain                     
				//This means we shove a blank transparent texture                    
				//into the starting buffer.  Easily done with memset(dest,0,sizeof); 
				memcpy(ourRAM,texture,mipSize * mipSize * sizeof(DWORD));
				forceAlphaOpaque(ourRAM,kmp);
			}
			else
				memset(ourRAM,0x01,mipSize * mipSize * sizeof(DWORD));

			//-------------------------------------------------------------
			// Create Binary number based on types which MATCH priType[1].
			DWORD binNumber = 0;
			if (priTypes[0] != priTypes[1])
			{
				binNumber = ((v0Type == priTypes[1])*8) + 
							((v1Type == priTypes[1])*4) + 
							((v2Type == priTypes[1])*2) + 
							((v3Type == priTypes[1]));

				//--------------------------------------------------------------
				// We now have the mask number, send texture to Combiner school
				// ONLY send it to school in the new universe if its type is cement.
				// Otherwise, NO!
				if (!Terrain::terrainTextures2 || isCementType(priTypes[1]))
					combineTxm(ourRAM,binNumber,priTypes[1],kmp);
			}

			if (priTypes[1] != priTypes[2])
			{
				binNumber = ((v0Type == priTypes[2])*8) + 
							((v1Type == priTypes[2])*4) + 
							((v2Type == priTypes[2])*2) + 
							((v3Type == priTypes[2]));

				//--------------------------------------------------------------
				// We now have the mask number, send texture to Combiner school
				// ONLY send it to school in the new universe if its type is cement.
				// Otherwise, NO!
				if (!Terrain::terrainTextures2 || isCementType(priTypes[2]))
					combineTxm(ourRAM,binNumber,priTypes[2],kmp);
			}

			if (priTypes[2] != priTypes[3])
			{
				binNumber = ((v0Type == priTypes[3])*8) + 
							((v1Type == priTypes[3])*4) + 
							((v2Type == priTypes[3])*2) + 
							((v3Type == priTypes[3]));

				//--------------------------------------------------------------
				// We now have the mask number, send texture to Combiner school
				// ONLY send it to school in the new universe if its type is cement.
				// Otherwise, NO!
				if (!Terrain::terrainTextures2 || isCementType(priTypes[3]))
					combineTxm(ourRAM,binNumber,priTypes[3],kmp);
			}

			if (overlayInfo != 0xffff)
			{
				//ONLY Combine certain overlays to prevent dumb things from old universe from showing up
				if (!Terrain::terrainTextures2)
					combineOverlayTxm(ourRAM,overlayInfo,kmp);
				else
				{
					if ((overlayInfo < 285) || (overlayInfo > 292))		//Don't draw the stupid rough stuff
						combineOverlayTxm(ourRAM,overlayInfo,kmp);
				}
			}
			
			//-------------------------------------------------
			// OurRAM at this point is the new magical Texture
			if (!Terrain::terrainTextures2)
				result = textureFromMemory(ourRAM,mipSize);
			else
				result = textureFromMemoryAlpha(ourRAM,mipSize);

			//-----------------------------------------------------------
			// TEST.....
			// What if we saved off all of the textures instead of combining
			// every time we load?  Should lower load time.  Lets see how big
			// for all of the textures...
			//
			if (!malformedCachedTransition)
			{
				char nameData[256];
				sprintf(nameData,"%08d.%08d.txm",typeInfo,overlayInfo);

				FullPathFileName testPath;
				testPath.init(texturePath,nameData,"");

				if (listTransitionFile)
				{
					listTransitionFile->writeString(testPath);
					listTransitionFile->writeByte(0);
				}

				MemoryPtr lzBuffer = (MemoryPtr)malloc(mipSize * mipSize * sizeof(DWORD) * 2);
				long bufferSize = LZCompress(lzBuffer,ourRAM,(mipSize * mipSize * sizeof(DWORD)));

				File txmFile;
				txmFile.create(testPath);
				txmFile.write(lzBuffer,bufferSize);
				txmFile.close();
				free(lzBuffer);
			}
				
			if (kmp == 0)
				txmResult = result;

			tileRAMHeap->Free(ourRAM);
		}
		transitions[nextTransition].transitionIndex = typeInfo;
		transitions[nextTransition].baseTXMIndex = txmResult;
		transitions[nextTransition].overlayIndex = overlayInfo;
		mc2TransitionTypeByTexture[txmResult] = typeInfo;
		mc2TransitionOverlayByTexture[txmResult] = overlayInfo;
		mc2TransitionInfoValid[txmResult] = true;
		nextTransition++;
	}

	return(txmResult);
}	

//---------------------------------------------------------------------------
long TerrainTextures::setTexture (DWORD typeInfo, DWORD overlayInfo)
{
	BYTE v0Type = typeInfo & 0x000000ff;
	BYTE v1Type = typeInfo >> 8;
	BYTE v2Type = typeInfo >> 16;
	BYTE v3Type = typeInfo >> 24;

	gosASSERT(v0Type < numTypes);
	gosASSERT(v1Type < numTypes);
	gosASSERT(v2Type < numTypes);
	gosASSERT(v3Type < numTypes);

	if ((v0Type == v1Type) && (v2Type == v3Type) && (v1Type == v3Type) && (overlayInfo == 0xffff))
	{
		long txmHandle = types[v0Type].baseTXMIndex;
		
		if (
			((v0Type == BASE_CEMENT_TYPE) ||
			((v0Type >= START_CEMENT_TYPE) && (v0Type <= END_CEMENT_TYPE))) ||
			((v1Type == BASE_CEMENT_TYPE) ||
			((v1Type >= START_CEMENT_TYPE) && (v1Type <= END_CEMENT_TYPE))) ||
			((v2Type == BASE_CEMENT_TYPE) ||
			((v2Type >= START_CEMENT_TYPE) && (v2Type <= END_CEMENT_TYPE))) ||
			((v3Type == BASE_CEMENT_TYPE) ||
			((v3Type >= START_CEMENT_TYPE) && (v3Type <= END_CEMENT_TYPE)))
			)
			textures[txmHandle].flags = MC2_TERRAIN_CEMENT_FLAG;		//No ALPHA.  Its a Solid cement texture.
		else
			textures[txmHandle].flags = 0;
			
		return(txmHandle);
	}
	else
	{
#ifdef USE_TRANSITIONS
		//------------------------------
		// Must create Transition here.
		// BUT NOW we only create them for CEMENT!!
		// New Terrain you know!!
		long txmHandle = 0xffffffff;
		if ( (overlayInfo != 0xffff) ||
			((v0Type == BASE_CEMENT_TYPE) ||
			((v0Type >= START_CEMENT_TYPE) && (v0Type <= END_CEMENT_TYPE))) ||
			((v1Type == BASE_CEMENT_TYPE) ||
			((v1Type >= START_CEMENT_TYPE) && (v1Type <= END_CEMENT_TYPE))) ||
			((v2Type == BASE_CEMENT_TYPE) ||
			((v2Type >= START_CEMENT_TYPE) && (v2Type <= END_CEMENT_TYPE))) ||
			((v3Type == BASE_CEMENT_TYPE) ||
			((v3Type >= START_CEMENT_TYPE) && (v3Type <= END_CEMENT_TYPE)))
			)
		{
			txmHandle = createTransition(typeInfo,overlayInfo);
			if (firstTransition == -1)
				firstTransition = txmHandle;
				
			textures[txmHandle].flags = MC2_TERRAIN_CEMENT_FLAG;
		}
		else
		{
			txmHandle = types[v0Type].baseTXMIndex;
			textures[txmHandle].flags = 0;
		}
			
		//Complex here.  If we are a cement texture and ANY other vxType was NOT cement, ALPHA is TRUE.
		// If we are a road and ANY other vxType was NOT cement, ALPHA is TRUE,
		// Otherwise, its false.  Thus, simply mark it true for any non-solid cement!!
		// We simply won't use anything that's not cement or road or runway, etc.
		if ((textures[txmHandle].flags & MC2_TERRAIN_CEMENT_FLAG) == MC2_TERRAIN_CEMENT_FLAG)
		{
			if (
				((v0Type != BASE_CEMENT_TYPE) &&
				(v0Type < START_CEMENT_TYPE) || (v0Type > END_CEMENT_TYPE))
				)
				textures[txmHandle].flags |= MC2_TERRAIN_ALPHA_FLAG;
				
			if (
				((v1Type != BASE_CEMENT_TYPE) &&
				(v1Type < START_CEMENT_TYPE) || (v1Type > END_CEMENT_TYPE))
				)
				textures[txmHandle].flags |= MC2_TERRAIN_ALPHA_FLAG;
	 
			if (
				((v2Type != BASE_CEMENT_TYPE) &&
				(v2Type < START_CEMENT_TYPE) || (v2Type > END_CEMENT_TYPE))
				)
				textures[txmHandle].flags |= MC2_TERRAIN_ALPHA_FLAG;
	 
			if (
				((v3Type != BASE_CEMENT_TYPE) &&
				(v3Type < START_CEMENT_TYPE) || (v3Type > END_CEMENT_TYPE))
				)
				textures[txmHandle].flags |= MC2_TERRAIN_ALPHA_FLAG;
		}
    		
#else
		long txmHandle = types[v0Type].baseTXMIndex;
#endif
		return(txmHandle);
	}
}							  

//---------------------------------------------------------------------------
long TerrainTextures::setDetail (DWORD typeInfo, DWORD frameNum)
{
	if (typeInfo < numDetails)
	{
		long txmHandle = details[typeInfo].baseTXMIndex + (frameNum * MC_MAX_MIP_LEVELS );
		return(txmHandle);
	}
	
	return 0xffffffff;
}

//---------------------------------------------------------------------------
void TerrainTextures::update (void)
{
	long i;
	if (tileRAMHeap)
	{
		//We can now safely purge all transitions cause the MC2 texture manager has them all!!
		numTypes = 0;
		types = NULL;

		numOverlays = 0;
		overlays = NULL;
		
		numTransitions = 0;
		transitions = NULL;
		nextTransition = 0;

		for (i=0;i<numDetails;i++)
		{
			details[i].detailName = NULL;
			details[i].detailData = NULL;
		}
			
		if (listTransitionFile)
		{
			listTransitionFile->close();
			delete listTransitionFile;
			listTransitionFile = NULL;
		}

		localBaseName = NULL;

		delete tileRAMHeap;
		tileRAMHeap = NULL;
	}
}

//---------------------------------------------------------------------------
void TerrainTextures::destroy (void)
{
	update();

	delete tileHeap;
	tileHeap = NULL;
}

//---------------------------------------------------------------------------
//		
//	Edit Log
//
//---------------------------------------------------------------------------
