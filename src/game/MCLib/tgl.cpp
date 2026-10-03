//-------------------------------------------------------------------------------
//
// Tiny Geometry Layer
//
// For MechCommander 2 -- Copyright (c) 1999 Microsoft
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef TGL_H
#include "tgl.h"
#include <chrono>   // PORT (M8-B15 perf): MC2_XFORM_CENSUS
#include <atomic>   // PORT (M8-B32): per-slot pool census across the object workers
#endif

#ifndef CLIP_H
#include "clip.h"
#endif

#ifndef TIMING_H
#include "Timing.h" // PORT: match case-sensitive host path.
#endif

#ifndef TERRAIN_H
#include "Terrain.h" // PORT: match case-sensitive host path.
#endif

#ifndef CAMERA_H
#include "Camera.h" // PORT: match case-sensitive host path.
#endif

#ifndef TXMMGR_H
#include "txmmgr.h"
#endif

//-------------------------------------------------------------------------------
// Include Files
#include <ToolOS.hpp> // PORT: match case-sensitive host path.

#define USE_ASSEMBLY
//-------------------------------------------------------------------------------
// Statics
Stuff::LinearMatrix4D 	*TG_Shape::cameraOrigin = NULL;
Stuff::Matrix4D			*TG_Shape::cameraToClip = NULL;
Stuff::LinearMatrix4D	TG_Shape::worldToCamera;
Stuff::Matrix4D			TG_Shape::worldToClip;

float					TG_Shape::viewMulX = 0.0;
float					TG_Shape::viewAddX = 0.0;
float					TG_Shape::viewMulY = 0.0;
float					TG_Shape::viewAddY = 0.0;

DWORD					TG_Shape::fogColor = 0xffffffff;
float					TG_Shape::fogStart = 0.0f;
float					TG_Shape::fogFull = 0.0f;

thread_local TG_LightPtr	*TG_Shape::listOfLights = NULL;
thread_local DWORD			TG_Shape::numLights = 0;
static thread_local TG_Light MC2_ParallelLights[MAX_LIGHTS_IN_WORLD];
static thread_local TG_LightPtr MC2_ParallelLightPtrs[MAX_LIGHTS_IN_WORLD];

thread_local Stuff::LinearMatrix4D TG_Shape::lightToShape[MAX_LIGHTS_IN_WORLD];
thread_local Stuff::Vector3D TG_Shape::lightDir[MAX_LIGHTS_IN_WORLD];
thread_local Stuff::Vector3D TG_Shape::rootLightDir[MAX_LIGHTS_IN_WORLD];
thread_local Stuff::Vector3D TG_Shape::spotDir[MAX_LIGHTS_IN_WORLD];

UserHeapPtr 			TG_Shape::tglHeap = NULL;

// PORT (M8-B9): see tgl.h. Zero is never a live generation, so a type that
// was never stamped always reads as stale rather than matching by accident.
unsigned long			TG_Shape::tglGeneration = 0;

DWORD					TG_Shape::lighteningLevel = 0;

thread_local TG_VertexPool 		*colorPool = NULL;
thread_local TG_GOSVertexPool 	*vertexPool = NULL;
thread_local TG_DWORDPool			*facePool = NULL;
thread_local TG_ShadowPool		*shadowPool = NULL;
thread_local TG_TrianglePool		*trianglePool = NULL;
thread_local bool MC2_TGLParallelWorker = false;

// PORT (M8-B32): which SET of pools this thread is drawing from.
//   0        = the main-thread pools built in mission.cpp
//   1..N     = parallel object worker N-1, from TG_PrepareParallelPools
// The main thread never calls TG_SelectParallelPool, so it keeps slot 0 even
// while it is running its own share of the parallel object pass.
thread_local long MC2_PoolSlot = 0;

namespace
{
	const long MC2_MAX_OBJECT_WORKERS = 8;
	// PORT (M8-B32): ONE set shared by every worker, not one set each. See the
	// measurement in TG_PrepareParallelPools.
	TG_VertexPool* MC2_WorkerColor = NULL;
	TG_GOSVertexPool* MC2_WorkerVertex = NULL;
	TG_DWORDPool* MC2_WorkerFace = NULL;
	TG_ShadowPool* MC2_WorkerShadow = NULL;
	TG_TrianglePool* MC2_WorkerTriangle = NULL;
	long MC2_WorkerPoolCount = 0;
	unsigned long MC2_WorkerPoolGeneration = 0;
}

bool TG_PrepareParallelPools (long workerCount)
{
	if (!TG_Shape::tglHeap || workerCount < 1 || workerCount > MC2_MAX_OBJECT_WORKERS)
		return false;
	if (MC2_WorkerPoolGeneration == TG_Shape::tglGeneration && MC2_WorkerPoolCount == workerCount)
		return true;

	// PORT (M8-B32): ONE SHARED POOL, NOT A FIXED SLICE PER WORKER.
	//
	// This used to hand each worker 48,000/workerCount units of its own, on the
	// reasoning that 48,000 cleared the measured terrain demand of 33,453
	// vertices. Both halves of that were true and the result still broke: the
	// terrain objects are dealt to the workers by a work-stealing queue, so the
	// split is decided at run time and is never even, and a worker that is
	// handed more than its slice gets NOTHING for the overflow. Every tree and
	// prop it was holding vanishes for that frame and comes back the next.
	//
	// The measurement that settled it, headless on mc2_06 with four workers,
	// camera at the top-left outpost at the horizon tilt M8-F1 opened up:
	//
	//   facing across the map   demand/turn per worker 23,023-26,598 verts
	//                           104,616 shapes refused, EVERY ONE in a worker
	//                           pool and NOT ONE in the main pool
	//   facing off the map edge  same camera, 25,099 refused
	//   the retail tilt floor    9,086 verts per worker, ZERO refused
	//
	// So aggregate capacity was 132,000 vertices against aggregate demand of
	// 97,747 and the frame still lost a hundred thousand shapes. Sizing was
	// never the problem; the partition was. Note also that the old code
	// allocated a slice for worker 0 and never used it -- the main thread runs
	// its own share of the pass without ever calling TG_SelectParallelPool, so
	// it draws from the MAIN pools, and a quarter of the budget sat idle. The
	// census now prints that slot so it cannot hide again.
	//
	// Sized against the worst camera measured, aggregate, with ~1.6x spare that
	// any worker can reach: 120,000 vertices against 73,705 wanted, 100,000
	// triangles against 60,464. Cost is exact -- TG_Vertex 4, gos_VERTEX 32,
	// shadow 16, TG_Triangle 24, DWORD 4 bytes -- 9.4 MB against the 4.0 MB the
	// four slices used, which is why tglHeapSize grows with it.
	//
	// facePool is drawn from TWICE per shape (visible faces AND visible
	// shadows), so it must stay at least 2x the triangle pool.
	//
	// MC2_TGL_POOL_VERTS shrinks it on purpose. That is not a tuning knob, it is
	// the A/B CONTROL: a captured frame taken with a starved pool against the
	// same frame taken with the shipped one is the only thing that shows this
	// defect as a PICTURE rather than as a counter.
	DWORD sharedVerts = 120000;
	{
		const char* e = getenv("MC2_TGL_POOL_VERTS");
		if (e && *e)
		{
			const long want = atol(e);
			if (want >= 1000 && want <= 1000000)
				sharedVerts = (DWORD)want;
		}
	}
	const DWORD sharedTris = (sharedVerts * 5) / 6;

	// The wrappers intentionally outlive mission heaps. On a new TGL generation
	// init overwrites their stale backing pointers; the old heap is already gone.
	if (!MC2_WorkerColor) MC2_WorkerColor = new TG_VertexPool;
	if (!MC2_WorkerVertex) MC2_WorkerVertex = new TG_GOSVertexPool;
	if (!MC2_WorkerFace) MC2_WorkerFace = new TG_DWORDPool;
	if (!MC2_WorkerShadow) MC2_WorkerShadow = new TG_ShadowPool;
	if (!MC2_WorkerTriangle) MC2_WorkerTriangle = new TG_TrianglePool;
	MC2_WorkerColor->init(sharedVerts);
	MC2_WorkerVertex->init(sharedVerts);
	MC2_WorkerFace->init(sharedTris * 2);
	MC2_WorkerShadow->init(sharedVerts);
	MC2_WorkerTriangle->init(sharedTris);
	MC2_PoolNoteCapacity(1, (long)sharedVerts, (long)sharedTris, (long)sharedTris * 2);

	MC2_WorkerPoolCount = workerCount;
	MC2_WorkerPoolGeneration = TG_Shape::tglGeneration;
	return true;
}

void TG_ResetParallelPools (void)
{
	if (!MC2_WorkerPoolCount)
		return;
	MC2_PoolNotePeak(1, MC2_WorkerVertex->getPeak(), MC2_WorkerTriangle->getPeak());
	MC2_WorkerColor->reset();
	MC2_WorkerVertex->reset();
	MC2_WorkerFace->reset();
	MC2_WorkerShadow->reset();
	MC2_WorkerTriangle->reset();
}

void TG_SelectParallelPool (long workerIndex)
{
	gosASSERT(workerIndex >= 0 && workerIndex < MC2_WorkerPoolCount);
	colorPool = MC2_WorkerColor;
	vertexPool = MC2_WorkerVertex;
	facePool = MC2_WorkerFace;
	shadowPool = MC2_WorkerShadow;
	trianglePool = MC2_WorkerTriangle;
	MC2_TGLParallelWorker = true;
	MC2_PoolSlot = workerIndex + 1;
}

//-------------------------------------------------------------------------------
extern bool useVertexLighting;
extern bool useFaceLighting;
extern bool hasGuardBand;
extern bool useFog;
extern DWORD BaseVertexColor;
bool drawOldWay = false;
extern bool useShadows;
bool useLocalShadows = false;

bool renderTGLShapes = true;

bool silentMode = false;		//Used for automated builds to keep errors from popping up.

//-------------------------------------------------------------------------------
// Parse Functions
void GetNumberData (char *rawData, char *result)
{
	long startIndex = 0;
	long endIndex = 0;
	while (	(rawData[startIndex] != '+') &&
			(rawData[startIndex] != '-') &&
			(rawData[startIndex] != '.') &&
			(rawData[startIndex] < '0') ||
			(rawData[startIndex] > '9'))
	{
		startIndex++;
	}

	endIndex = startIndex;
	while (	(rawData[endIndex] == '+') ||
			(rawData[endIndex] == '-') ||
			(rawData[endIndex] == '.') ||
			(rawData[endIndex] >= '0') &&
			(rawData[endIndex] <= '9'))
	{
		endIndex++;
	}

	strncpy(result,&rawData[startIndex],endIndex - startIndex);
	result[endIndex-startIndex] = 0;
}	

//-------------------------------------------------------------------------------
void GetWordData (char *rawData, char *result)
{
	long startIndex = 0;
	long endIndex = 0;
	while (	(rawData[startIndex] < 'A') ||
			(rawData[startIndex] > 'Z') &&
			(rawData[startIndex] < 'a') ||
			(rawData[startIndex] > 'z'))
	{
		startIndex++;
	}

	endIndex = startIndex;
	while (	((rawData[endIndex] >= 'A') && (rawData[endIndex] <= 'Z')) ||
			(rawData[endIndex] == '_') ||
			((rawData[endIndex] >= 'a') && (rawData[endIndex] <= 'z')) )
	{
		endIndex++;
	}

	strncpy(result,&rawData[startIndex],endIndex - startIndex);
	result[endIndex-startIndex] = 0;
}	

//-------------------------------------------------------------------------------
void GetNameData (char *rawData, char *result)
{
	long startIndex = 0;
	long endIndex = 0;
	while (	(rawData[startIndex] != '"') )
	{
		startIndex++;
	}

	startIndex++;
	endIndex = startIndex;
	while (	(rawData[endIndex] != '"') )
	{
		endIndex++;
	}

	strncpy(result,&rawData[startIndex],endIndex - startIndex);
	result[endIndex-startIndex] = 0;
}	

//-------------------------------------------------------------------------------
// Class TG_TypeNode

//-------------------------------------------------------------------------------
 void *TG_TypeNode::operator new (size_t mySize)
 {
	void *result = TG_Shape::tglHeap->Malloc(mySize);
	return result;
 }
 
//-------------------------------------------------------------------------------
 void TG_TypeNode::operator delete (void *us)
 {
	TG_Shape::tglHeap->Free(us);
 }
 
//-------------------------------------------------------------------------------
// Frees memory and resets locals to defaults.
void TG_TypeNode::destroy (void)
{
	init();
}

//-------------------------------------------------------------------------------
// This function creates an instance of a TG_Shape from a TG_TypeNode.  Saves RAM.
TG_ShapePtr TG_TypeNode::CreateFrom (void)
{
	long i;
	TG_ShapePtr newShape = NULL;
	newShape = (TG_ShapePtr)TG_Shape::tglHeap->Malloc(sizeof(TG_Shape));
	gosASSERT(newShape != NULL);
	
	//listOfVertices
	newShape->numVertices = 0;
	newShape->listOfColors = NULL;
	newShape->listOfVertices = NULL;
	newShape->listOfShadowVertices = NULL;

	//listOfTriangles
	newShape->numTriangles = 0;
	newShape->listOfTriangles = NULL;

	//listOfVisibleFaces
	newShape->numVisibleFaces = 0;
	newShape->listOfVisibleFaces = NULL;

	//listOfVisibleShadows
	newShape->numVisibleShadows = 0;
	newShape->listOfVisibleShadows = NULL;

	//Other Data
	newShape->myType = this;
	if (newShape->myType == NULL)
		STOP(("ShapeType NULL.  BAD Shape in MAX FILE"));
		
	newShape->aRGBHighlight = 0x00000000;
	newShape->lightsOut = false;
	newShape->recalcShadows = true;
	newShape->shapeScalar = 0.0f;

	for (i=0;i<MAX_SHADOWS;i++)
		newShape->shadowsVisible[i] = false;

	newShape->isSpotlight = (strnicmp(newShape->getNodeName(),"SpotLight_",10) == 0);
	newShape->isWindow = (strnicmp(newShape->getNodeName(),"LitWin_",6) == 0);

	if (newShape->isSpotlight)
		newShape->noShadow = true;
	else
		newShape->noShadow = false;

	return newShape;
}

//-------------------------------------------------------------------------------
//Function return 0 is OK.  -1 if file is not ASE Format or missing data.
//This function simply parses the ASE buffers handed to it.  This allows
//users to load the ase file themselves and manage their own memory for it.
//It allocates memory for internal Lists.  These are straight tglHeap->Mallocs at present.
//
long TG_TypeNode::MakeFromHelper (BYTE *aseBuffer, char *fileName)
{
	//------------------------------------------
	// Store off the Node Names.
	char *nodeName = strstr((char *)aseBuffer,ASE_NODE_NAME);
	gosASSERT(nodeName != NULL);

	char nodeString[1024];
	nodeName += strlen(ASE_NODE_NAME)+1;
	GetNameData(nodeName,nodeString);
	
#ifdef _DEBUG
	if (strlen(nodeString) >= TG_NODE_ID)
	{
		if (!silentMode)
			PAUSE(("WARNING: Node ID %s in Shape %s is greater then 24 characters!!",nodeString,fileName));
	}
#endif

	strncpy(nodeId,nodeString,24);
		 
	char* parentName = strstr((char *)nodeName,ASE_NODE_PARENT);

	//-------------------------------------------------------------------
	// Must also check to make sure we HAVE a parent.
	// We will get the next GeomObject's parent if we don't check length!
	if ((parentName != NULL) && ((parentName - nodeName) < MAX_SCAN_LENGTH))
	{
		parentName += strlen(ASE_NODE_PARENT)+1;
		GetNameData(parentName,nodeString);
		
		strncpy(parentId,nodeString,24);
	}
	else
	{
		strcpy(parentId,"None");
	}

	//----------------------------------------------------
	// Store off NODE ABS position for heirarchy
	nodeName = strstr((char *)aseBuffer,ASE_NODE_POS);
	gosASSERT(nodeName != NULL);

	nodeName += strlen(ASE_NODE_POS)+1;

	char numData[512];

	GetNumberData(nodeName,numData);
	nodeCenter.x = -(float)atof(numData);
	nodeName += strlen(numData)+1;

	GetNumberData(nodeName,numData);
	nodeCenter.z = (float)atof(numData);
	nodeName += strlen(numData)+1;

	GetNumberData(nodeName,numData);
	nodeCenter.y = (float)atof(numData);

	return(0);
}	

//-------------------------------------------------------------------------------
void TG_TypeNode::LoadBinaryCopy (File &binFile)
{
	//Other Data
	nodeCenter.x = binFile.readFloat();
	nodeCenter.y = binFile.readFloat();
	nodeCenter.z = binFile.readFloat();
	
	relativeNodeCenter.x = binFile.readFloat();
	relativeNodeCenter.y = binFile.readFloat();
	relativeNodeCenter.z = binFile.readFloat();
	
	binFile.read((MemoryPtr)nodeId,TG_NODE_ID);
	binFile.read((MemoryPtr)parentId,TG_NODE_ID);
}

//-------------------------------------------------------------------------------
void TG_TypeNode::SaveBinaryCopy (File &binFile)
{
	//What kind of thing is this
	binFile.writeLong(TYPE_NODE);
	
	//Other Data
	binFile.writeFloat(nodeCenter.x);
	binFile.writeFloat(nodeCenter.y);
	binFile.writeFloat(nodeCenter.z);
	
	binFile.writeFloat(relativeNodeCenter.x);
	binFile.writeFloat(relativeNodeCenter.y);
	binFile.writeFloat(relativeNodeCenter.z);
	
	binFile.write((MemoryPtr)nodeId,TG_NODE_ID);
	binFile.write((MemoryPtr)parentId,TG_NODE_ID);
}

//-------------------------------------------------------------------------------
// Class TG_Shape && TG_TypeShape

//-------------------------------------------------------------------------------
// TG_TypeShape
// This function creates an instance of a TG_Shape from a TG_TypeShape.  Saves RAM.
TG_ShapePtr TG_TypeShape::CreateFrom (void)
{
	long i;
	TG_ShapePtr newShape = NULL;
	newShape = (TG_ShapePtr)TG_Shape::tglHeap->Malloc(sizeof(TG_Shape));
	gosASSERT(newShape != NULL);
	
	//listOfVertices
	newShape->numVertices = numTypeVertices;
	if (numTypeVertices)
	{
		//Mark with invalid pointer so we now to get RAM from POOL! 	
		newShape->listOfVertices = NULL;
		newShape->listOfColors = NULL;
	}
	else
	{
		//Otherwise mark with NULL so we know not to reference!!
		newShape->listOfVertices = NULL;
		newShape->listOfColors = NULL;
	}

	//listOfShadowVertices
	if (numTypeVertices)
	{
		//These are UNIQUE to each instance and do not change much per frame.
		// Thus, store them between frames to save processing time!
		newShape->listOfShadowVertices = (TG_ShadowVertexPtr)TG_Shape::tglHeap->Malloc(sizeof(TG_ShadowVertex) * numTypeVertices * MAX_SHADOWS);
		gosASSERT(newShape->listOfShadowVertices != NULL);

		memset(newShape->listOfShadowVertices,0xff,sizeof(TG_ShadowVertex) * numTypeVertices * MAX_SHADOWS);
		
		//Mark with invalid pointer so we now to get RAM from POOL! 	
		newShape->listOfShadowTVertices = NULL;
	}
	else
	{
		//Otherwise mark with NULL so we know not to reference!!
		newShape->listOfShadowVertices = NULL;
		newShape->listOfShadowTVertices = NULL;
	}

	//listOfTriangles
	newShape->numTriangles = numTypeTriangles;
	if (numTypeTriangles)
	{
		//Mark with invalid pointer so we now to get RAM from POOL! 	
		newShape->listOfTriangles = NULL;
	}
	else
	{
		//Otherwise mark with NULL so we know not to reference!!
		newShape->listOfTriangles = NULL;
	}

	//listOfVisibleFaces
	newShape->numVisibleFaces = 0;
	if (numTypeTriangles)
	{
		//Mark with invalid pointer so we now to get RAM from POOL! 	
		newShape->listOfVisibleFaces = NULL;
	}
	else
	{
		//Otherwise mark with NULL so we know not to reference!!
		newShape->listOfVisibleFaces = NULL;
	}

	//listOfVisibleShadows
	newShape->numVisibleShadows = 0;
	if (numTypeTriangles)
	{
		//Mark with invalid pointer so we now to get RAM from POOL! 	
		newShape->listOfVisibleShadows = NULL;
	}
	else
	{
		//Otherwise mark with NULL so we know not to reference!!
		newShape->listOfVisibleShadows = NULL;
	}

	//Other Data
	newShape->myType = this;
	if (newShape->myType == NULL)
		STOP(("ShapeType NULL.  BAD Shape in MAX FILE"));
 	
	newShape->aRGBHighlight = 0x00000000;
	newShape->lightsOut = false;
	newShape->recalcShadows = true;
	newShape->shapeScalar = 0.0f;

	for (i=0;i<MAX_SHADOWS;i++)
		newShape->shadowsVisible[i] = false;
		
	newShape->isSpotlight = (strnicmp(newShape->getNodeName(),"SpotLight_",10) == 0);
	newShape->isWindow = (strnicmp(newShape->getNodeName(),"LitWin_",6) == 0);

	if (newShape->isSpotlight)
		newShape->noShadow = true;
	else
		newShape->noShadow = false;

	return newShape;
}

//-------------------------------------------------------------------------------
void TG_TypeShape::LoadBinaryCopy (File &binFile)
{
	//listOfTypeVertices
	numTypeVertices = binFile.readLong();
	if (numTypeVertices)
	{
		listOfTypeVertices = (TG_TypeVertexPtr)TG_Shape::tglHeap->Malloc(sizeof(TG_TypeVertex) * numTypeVertices);
		gosASSERT(listOfTypeVertices != NULL);

		binFile.read((MemoryPtr)listOfTypeVertices,sizeof(TG_TypeVertex) * numTypeVertices);
	}
	else
	{
		listOfTypeVertices = NULL;
	}

	//listOfTypeTriangles
	numTypeTriangles = binFile.readLong();
	if (numTypeTriangles)
	{
		listOfTypeTriangles = (TG_TypeTrianglePtr)TG_Shape::tglHeap->Malloc(sizeof(TG_TypeTriangle) * numTypeTriangles);
		gosASSERT(listOfTypeTriangles != NULL);

		binFile.read((MemoryPtr)listOfTypeTriangles,sizeof(TG_TypeTriangle) * numTypeTriangles);
	}
	else
	{
		listOfTypeTriangles = NULL;
	}

	//listOfTextures
	numTextures = binFile.readLong();
	if (numTextures)
	{
		listOfTextures = (TG_TinyTexturePtr)TG_Shape::tglHeap->Malloc(sizeof(TG_TinyTexture) * numTextures);
		gosASSERT(listOfTextures != NULL);

		binFile.read((MemoryPtr)listOfTextures,sizeof(TG_TinyTexture) * numTextures);
	}
	else
	{
		listOfTextures = NULL;
	}

	//Other Data
	nodeCenter.x = binFile.readFloat();
	nodeCenter.y = binFile.readFloat();
	nodeCenter.z = binFile.readFloat();
	
	relativeNodeCenter.x = binFile.readFloat();
	relativeNodeCenter.y = binFile.readFloat();
	relativeNodeCenter.z = binFile.readFloat();
	
	alphaTestOn = false;
	filterOn = true;

	binFile.read((MemoryPtr)nodeId,TG_NODE_ID);
	binFile.read((MemoryPtr)parentId,TG_NODE_ID);
}

//-------------------------------------------------------------------------------
void TG_TypeShape::SaveBinaryCopy (File &binFile)
{
	//What kind of thing is this
	binFile.writeLong(SHAPE_NODE);
	
	//listOfTypeVertices
	binFile.writeLong(numTypeVertices);
	if (numTypeVertices)
	{
		binFile.write((MemoryPtr)listOfTypeVertices,sizeof(TG_TypeVertex) * numTypeVertices);
	}

	//listOfTypeTriangles
	binFile.writeLong(numTypeTriangles);
	if (numTypeTriangles)
	{
		binFile.write((MemoryPtr)listOfTypeTriangles,sizeof(TG_TypeTriangle) * numTypeTriangles);
	}

	//listOfTextures
	binFile.writeLong(numTextures);
	if (numTextures)
	{
		binFile.write((MemoryPtr)listOfTextures,sizeof(TG_TinyTexture) * numTextures);
	}

	//Other Data
	binFile.writeFloat(nodeCenter.x);
	binFile.writeFloat(nodeCenter.y);
	binFile.writeFloat(nodeCenter.z);
	
	binFile.writeFloat(relativeNodeCenter.x);
	binFile.writeFloat(relativeNodeCenter.y);
	binFile.writeFloat(relativeNodeCenter.z);
	
	binFile.write((MemoryPtr)nodeId,TG_NODE_ID);
	binFile.write((MemoryPtr)parentId,TG_NODE_ID);
}

//-------------------------------------------------------------------------------
//Frees memory and resets locals to defaults.
void TG_TypeShape::destroy (void)
{
	if (listOfTypeVertices)
		TG_Shape::tglHeap->Free(listOfTypeVertices);
	listOfTypeVertices = NULL;

	if (listOfTypeTriangles)
		TG_Shape::tglHeap->Free(listOfTypeTriangles);
	listOfTypeTriangles = NULL;

	if (listOfTextures)
		TG_Shape::tglHeap->Free(listOfTextures);
	listOfTextures = NULL;

	numTypeVertices = numTypeTriangles = numTextures = 0;
}	

//-------------------------------------------------------------------------------
//Frees memory and resets locals to defaults.
void TG_Shape::destroy (void)
{
	//THIS IS CORRECT!!!!
	// They come from a common pool now, Deleteing them would be BAD!!
	listOfVertices = NULL;
	listOfColors = NULL;
	listOfTriangles = NULL;
	listOfVisibleFaces = NULL;
	listOfVisibleShadows = NULL;
	listOfShadowTVertices = NULL;

	if (listOfShadowVertices)
		tglHeap->Free(listOfShadowVertices);
	listOfShadowVertices = NULL;

	numVertices = numTriangles = numVisibleFaces = 0;
}	

//-------------------------------------------------------------------------------
//Function return 0 is OK.  -1 if file is not ASE Format or missing data.
//This function simply parses the ASE buffers handed to it.  This allows
//users to load the ase file themselves and manage their own memory for it.
//It allocates memory for internal Lists.  These are straight tglHeap->Mallocs at present.
//
// NOTE: Only takes the first HELPOBJECT from the ASE file.  Multi-object
// Files will require user intervention to parse!!
long TG_TypeShape::MakeFromHelper (BYTE *aseBuffer, char *fileName)
{
	//------------------------------------------
	// Store off the Node Names.
	char *nodeName = strstr((char *)aseBuffer,ASE_NODE_NAME);
	gosASSERT(nodeName != NULL);

	char nodeString[1024];
	nodeName += strlen(ASE_NODE_NAME)+1;
	GetNameData(nodeName,nodeString);
	
#ifdef _DEBUG
	if (strlen(nodeString) >= TG_NODE_ID)
	{
		if (!silentMode)
			PAUSE(("WARNING: Node ID %s in Shape %s is greater then 24 characters!!",nodeString,fileName));
	}
#endif

	strncpy(nodeId,nodeString,24);
		 
	char* parentName = strstr((char *)nodeName,ASE_NODE_PARENT);

	//-------------------------------------------------------------------
	// Must also check to make sure we HAVE a parent.
	// We will get the next GeomObject's parent if we don't check length!
	if ((parentName != NULL) && ((parentName - nodeName) < MAX_SCAN_LENGTH))
	{
		parentName += strlen(ASE_NODE_PARENT)+1;
		GetNameData(parentName,nodeString);
		
		strncpy(parentId,nodeString,24);
	}
	else
	{
		strcpy(parentId,"None");
	}

	//----------------------------------------------------
	// Store off NODE ABS position for heirarchy
	nodeName = strstr((char *)aseBuffer,ASE_NODE_POS);
	gosASSERT(nodeName != NULL);

	nodeName += strlen(ASE_NODE_POS)+1;

	char numData[512];

	GetNumberData(nodeName,numData);
	nodeCenter.x = -(float)atof(numData);
	nodeName += strlen(numData)+1;

	GetNumberData(nodeName,numData);
	nodeCenter.z = (float)atof(numData);
	nodeName += strlen(numData)+1;

	GetNumberData(nodeName,numData);
	nodeCenter.y = (float)atof(numData);

	//----------------------------------------
	// Find the Number of Vertices
	numTypeVertices = 0;

	//---------------------------------------
	// Find the Number of faces
	numTypeTriangles = 0;

	return(0);
}	

//-------------------------------------------------------------------------------
//Function return 0 is OK.  -1 if file is not ASE Format or missing data.
//This function simply parses the ASE buffers handed to it.  This allows
//users to load the ase file themselves and manage their own memory for it.
//It allocates memory for internal Lists.  These are straight tglHeap->Mallocs at present.
//
// NOTE: Only takes the first GEOMOBJECT from the ASE file.  Multi-object
// Files will require user intervention to parse!!
long TG_TypeShape::ParseASEFile (BYTE *aseBuffer, char *fileName)
{
	long i;
	//------------------------------------------
	// Store off the Node Names.
	char *nodeName = strstr((char *)aseBuffer,ASE_NODE_NAME);
	gosASSERT(nodeName != NULL);

	char nodeString[1024];
	nodeName += strlen(ASE_NODE_NAME)+1;
	GetNameData(nodeName,nodeString);

#ifdef _DEBUG
	if (strlen(nodeString) >= TG_NODE_ID)
	{
		if (!silentMode)
			PAUSE(("WARNING: Node ID %s in Shape %s is greater then 24 characters!!",nodeString,fileName));
	}
#endif

	strncpy(nodeId,nodeString,24);
	
	char* parentName = strstr((char *)nodeName,ASE_NODE_PARENT);

	//-------------------------------------------------------------------
	// Must also check to make sure we HAVE a parent.
	// We will get the next GeomObject's parent if we don't check length!
	if ((parentName != NULL) && ((parentName - nodeName) < MAX_SCAN_LENGTH))
	{
		parentName += strlen(ASE_NODE_PARENT)+1;
		GetNameData(parentName,nodeString);
		
		strncpy(parentId,nodeString,24);
	}
	else
	{
		strcpy(parentId,"None");
	}

	//----------------------------------------------------
	// Store off NODE ABS position for heirarchy
	nodeName = strstr((char *)aseBuffer,ASE_NODE_POS);
	gosASSERT(nodeName != NULL);

	nodeName += strlen(ASE_NODE_POS)+1;

	char numData[512];

	GetNumberData(nodeName,numData);
	nodeCenter.x = -(float)atof(numData);
	nodeName += strlen(numData)+1;

	GetNumberData(nodeName,numData);
	nodeCenter.z = (float)atof(numData);
	nodeName += strlen(numData)+1;

	GetNumberData(nodeName,numData);
	nodeCenter.y = (float)atof(numData);

	//----------------------------------------
	// Find the Number of Vertices
	char *vertexCount = strstr((char *)aseBuffer,ASE_NUM_VERTEX);
	gosASSERT(vertexCount != NULL);
		
	vertexCount += strlen(ASE_NUM_FACE)+1;
	numTypeVertices = atol(vertexCount);
	if (numTypeVertices == 0)			//NO Data for this node.  WARN and then do NOT allocate anything!
	{
#ifdef _DEBUG
		if (!silentMode)
			PAUSE(("WARNING: No Vertices for Shape Node %s in Shape %s!",nodeId,fileName));
#endif
		init();

		return (0);
	}

	//---------------------------------------
	// Find the Number of faces
	char *faceCount = strstr((char *)aseBuffer,ASE_NUM_FACE);
	gosASSERT(faceCount != NULL);
		
	faceCount += strlen(ASE_NUM_FACE)+1;
	numTypeTriangles = atol(faceCount);
	gosASSERT(numTypeTriangles != 0);

	//-------------------
	// Vertex Data Next.
	listOfTypeVertices = (TG_TypeVertexPtr)TG_Shape::tglHeap->Malloc(sizeof(TG_TypeVertex) * numTypeVertices);
	gosASSERT(listOfTypeVertices != NULL);

	memset(listOfTypeVertices,0xff,sizeof(TG_TypeVertex) * numTypeVertices);

	//---------------------------
	// Create vertex normal list
	long *vertexNormalCount = (long *)TG_Shape::tglHeap->Malloc(sizeof(long) * numTypeVertices);
	gosASSERT(vertexNormalCount != NULL);

	memset(vertexNormalCount,0,sizeof(long) * numTypeVertices);

	for (i=0;i<numTypeVertices;i++)
	{
		char numberData[256];

		//------------------------------------------------
		// First the Vertex Position
		char vertexID[256];
		sprintf(vertexID,"%s% 5d",ASE_MESH_VERTEX_ID,i);

		char *vertexData = strstr((char *)aseBuffer,vertexID);
		gosASSERT(vertexData != NULL);
			
		vertexData += strlen(vertexID)+1;

		GetNumberData(vertexData,numberData);
		vertexData += strlen(numberData)+1;
		listOfTypeVertices[i].position.x = -(float)atof(numberData);

		GetNumberData(vertexData,numberData);
		vertexData += strlen(numberData)+1;
		listOfTypeVertices[i].position.z = (float)atof(numberData);

		GetNumberData(vertexData,numberData);
		listOfTypeVertices[i].position.y = (float)atof(numberData);

		//------------------------------------------------
		// Next, Vertex Normal
		// These are "funny" because MAX writes out a
		// separate normal for each instance of the vertex
		// wrt a face.  So we must catch EVERY instance
		// and average together and then normalize.
		// As such, we must now extract during the Face part below
		// Just init the normal here!
		listOfTypeVertices[i].normal.x = 
		listOfTypeVertices[i].normal.y = 
		listOfTypeVertices[i].normal.z = 0.0f;

		//-----------------------------------------------------
		// Next, Read in Vertex Colors

		listOfTypeVertices[i].aRGBLight 			= 0xff000000;

		//-----------------------------------------------------
	}

	//---------------------------------------
	// Face Data Next.
	listOfTypeTriangles = (TG_TypeTrianglePtr)TG_Shape::tglHeap->Malloc(sizeof(TG_TypeTriangle) * numTypeTriangles);
	gosASSERT(listOfTypeTriangles != NULL);

	memset(listOfTypeTriangles,0xff,sizeof(TG_TypeTriangle) * numTypeTriangles);

	for (i=0;i<numTypeTriangles;i++)
	{
		char numberData[256];

		//-----------------------------------------------
		// First the Vertices for the face
		char faceId[256];
		sprintf(faceId,"%s% 5d:",ASE_MESH_FACE_ID,i);

		char *faceData = strstr((char *)aseBuffer,faceId);
		gosASSERT(faceData != NULL);

		faceData = strstr(faceData,"A:");
		gosASSERT(faceData != NULL);
			
		GetNumberData(faceData,numberData);
		listOfTypeTriangles[i].Vertices[0] = atol(numberData);

		faceData = strstr(faceData,"B:");
		gosASSERT(faceData != NULL);

		GetNumberData(faceData,numberData);
		listOfTypeTriangles[i].Vertices[1] = atol(numberData);

		faceData = strstr(faceData,"C:");
		gosASSERT(faceData != NULL);
	
		GetNumberData(faceData,numberData);
		listOfTypeTriangles[i].Vertices[2] = atol(numberData);

		if ((numTextures/2) == 1)		//We don't need the MATLID, its always 0.  Now divBytwo to load shadow texture as well.
			listOfTypeTriangles[i].localTextureHandle = 0;
		else
		{
			//Must parse MATLID to find material ID.
			faceData = strstr(faceData,ASE_FACE_MATERIAL_ID);
			gosASSERT(faceData != NULL);

			faceData += strlen(ASE_FACE_MATERIAL_ID)+1;

			GetNumberData(faceData,numberData);

			long matLid = atol(numberData);
			if (matLid >= (numTextures/2))
				matLid = (numTextures/2) - 1;

			listOfTypeTriangles[i].localTextureHandle = matLid;

		}

		listOfTypeTriangles[i].renderStateFlags = 0;		//No Flags yet.

		//---------------------------------------------------------------------
		// UVData for the face.
		// This should be nummies!
		//
		//Load up the TFaces
		sprintf(faceId,"%s",ASE_NUM_TVFACES);

		faceData = strstr((char *)aseBuffer,faceId);
		if (faceData)
		{
			faceData += strlen(faceId);
			GetNumberData(faceData,numberData);
	
			Stuff::Vector3D tVFaces;
	
			sprintf(faceId,"%s %d",ASE_MESH_TFACE_ID,i);
	
			faceData = strstr(faceData,faceId);
			gosASSERT(faceData != NULL);
	
			faceData += strlen(faceId);
				
			GetNumberData(faceData,numberData);
			tVFaces.x = (float)atol(numberData);
			faceData += strlen(numberData)+1;
				
			GetNumberData(faceData,numberData);
			tVFaces.y = (float)atol(numberData);
			faceData += strlen(numberData)+1;
	
			GetNumberData(faceData,numberData);
			tVFaces.z = (float)atol(numberData);
	
			//Load up the TVERT0
			sprintf(faceId,"%s",ASE_NUM_TVERTEX);
	
			faceData = strstr((char *)aseBuffer,faceId);
			gosASSERT(faceData != NULL);
	
			sprintf(faceId,"%s %d",ASE_MESH_TVERT_ID,(long)tVFaces.x);
	
			faceData = strstr(faceData,faceId);
			gosASSERT(faceData != NULL);
	
			faceData += strlen(faceId);
				
			GetNumberData(faceData,numberData);
			listOfTypeTriangles[i].uvdata.u0 = (float)atof(numberData);
	
			if ((listOfTypeTriangles[i].uvdata.u0 > 100.0f) || (listOfTypeTriangles[i].uvdata.u0 < -100.0f))
			{
#ifdef _DEBUG
				if (!silentMode)
					PAUSE(("WARNING: U0 is %f which is out of range in Shape %s!",listOfTypeTriangles[i].uvdata.u0,fileName));
#endif
				listOfTypeTriangles[i].uvdata.u0 = 0.0f;
			}
	
			faceData += strlen(numberData)+1;
				
			GetNumberData(faceData,numberData);
			listOfTypeTriangles[i].uvdata.v0 = 1.0f - (float)atof(numberData);
	
			if ((listOfTypeTriangles[i].uvdata.v0 > 100.0f) || (listOfTypeTriangles[i].uvdata.v0 < -100.0f))
			{
#ifdef _DEBUG
				if (!silentMode)
					PAUSE(("WARNING: V0 is %f which is out of range in Shape %s!",listOfTypeTriangles[i].uvdata.v0,fileName));
#endif
				listOfTypeTriangles[i].uvdata.v0 = 0.0f;
			}
	
			//Load up the TVERT1
			sprintf(faceId,"%s",ASE_NUM_TVERTEX);
	
			faceData = strstr((char *)aseBuffer,faceId);
			gosASSERT(faceData != NULL);
	
			sprintf(faceId,"%s %d",ASE_MESH_TVERT_ID,(long)tVFaces.y);
	
			faceData = strstr(faceData,faceId);
			gosASSERT(faceData != NULL);
	
			faceData += strlen(faceId);
				
			GetNumberData(faceData,numberData);
			listOfTypeTriangles[i].uvdata.u1 = (float)atof(numberData);
			if ((listOfTypeTriangles[i].uvdata.u1 > 100.0f) || (listOfTypeTriangles[i].uvdata.u1 < -100.0f))
			{
#ifdef _DEBUG
				if (!silentMode)
					PAUSE(("WARNING: U1 is %f which is out of range in Shape %s!",listOfTypeTriangles[i].uvdata.u1,fileName));
#endif
				listOfTypeTriangles[i].uvdata.u1 = 0.0f;
			}
	
			faceData += strlen(numberData)+1;
				
			GetNumberData(faceData,numberData);
			listOfTypeTriangles[i].uvdata.v1 = 1.0f - (float)atof(numberData);
			if ((listOfTypeTriangles[i].uvdata.v1 > 100.0f) || (listOfTypeTriangles[i].uvdata.v1 < -100.0f))
			{
#ifdef _DEBUG
				if (!silentMode)
					PAUSE(("WARNING: V1 is %f which is out of range in Shape %s!",listOfTypeTriangles[i].uvdata.v1,fileName));
#endif
				listOfTypeTriangles[i].uvdata.v1 = 0.0f;
			}
	
			//Load up the TVERT2
			sprintf(faceId,"%s",ASE_NUM_TVERTEX);
	
			faceData = strstr((char *)aseBuffer,faceId);
			gosASSERT(faceData != NULL);
	
			sprintf(faceId,"%s %d",ASE_MESH_TVERT_ID,(long)tVFaces.z);
	
			faceData = strstr(faceData,faceId);
			gosASSERT(faceData != NULL);
	
			faceData += strlen(faceId);
				
			GetNumberData(faceData,numberData);
			listOfTypeTriangles[i].uvdata.u2 = (float)atof(numberData);
			if ((listOfTypeTriangles[i].uvdata.u2 > 100.0f) || (listOfTypeTriangles[i].uvdata.u2 < -100.0f))
			{
#ifdef _DEBUG
				if (!silentMode)
					PAUSE(("WARNING: U2 is %f which is out of range in Shape %s!",listOfTypeTriangles[i].uvdata.u2,fileName));
#endif
				listOfTypeTriangles[i].uvdata.u2 = 0.0f;
			}
	
			faceData += strlen(numberData)+1;
				
			GetNumberData(faceData,numberData);
			listOfTypeTriangles[i].uvdata.v2 = 1.0f - (float)atof(numberData);
			if ((listOfTypeTriangles[i].uvdata.v2 > 100.0f) || (listOfTypeTriangles[i].uvdata.v2 < -100.0f))
			{
#ifdef _DEBUG
				if (!silentMode)
					PAUSE(("WARNING: V2 is %f which is out of range in Shape %s!",listOfTypeTriangles[i].uvdata.v2,fileName));
#endif
				listOfTypeTriangles[i].uvdata.v2 = 0.0f;
			}
		}
		else
		{
			listOfTypeTriangles[i].uvdata.u0 = 
			listOfTypeTriangles[i].uvdata.v0 = 
   			listOfTypeTriangles[i].uvdata.u1 = 
   			listOfTypeTriangles[i].uvdata.v1 = 
   			listOfTypeTriangles[i].uvdata.u2 = 
   			listOfTypeTriangles[i].uvdata.v2 = 0.0f;
		}

		//------------------------------------------------------------

		//---------------------------------------------------------------------
		// Color Data for the face.
		//Load up the CFaces
		sprintf(faceId,"%s",ASE_NUM_CVFACES);

		faceData = strstr((char *)aseBuffer,faceId);
		if (faceData)
		{
			gosASSERT(faceData != NULL);

			faceData += strlen(faceId);
			GetNumberData(faceData,numberData);

			Stuff::Vector3D cVFaces;

			sprintf(faceId,"%s %d",ASE_MESH_CFACE_ID,i);

			faceData = strstr(faceData,faceId);
			gosASSERT(faceData != NULL);

			faceData += strlen(faceId);
				
			GetNumberData(faceData,numberData);
			cVFaces.x = (float)atol(numberData);
			faceData += strlen(numberData)+1;
				
			GetNumberData(faceData,numberData);
			cVFaces.y = (float)atol(numberData);
			faceData += strlen(numberData)+1;

			GetNumberData(faceData,numberData);
			cVFaces.z = (float)atol(numberData);

			//Load up the VERTCOL0
			sprintf(faceId,"%s",ASE_NUM_CVERTEX);

			faceData = strstr((char *)aseBuffer,faceId);
			gosASSERT(faceData != NULL);

			sprintf(faceId,"%s %d",ASE_MESH_VERTCOL_ID,(long)cVFaces.x);

			faceData = strstr(faceData,faceId);
			gosASSERT(faceData != NULL);

			faceData += strlen(faceId);
				
			GetNumberData(faceData,numberData);
			float red = (float)atof(numberData);

			faceData += strlen(numberData)+1;
				
			GetNumberData(faceData,numberData);
			float green = (float)atof(numberData);

			faceData += strlen(numberData)+1;

			GetNumberData(faceData,numberData);
			float blue = (float)atof(numberData);

			DWORD redC = red * 0xff;
			DWORD greenC = green * 0xff;
			DWORD blueC = blue * 0xff;

			listOfTypeVertices[listOfTypeTriangles[i].Vertices[0]].aRGBLight = (0xff << 24) + (redC << 16) + (greenC << 8) + blueC;

			//Load up the VERTCOL1
			sprintf(faceId,"%s",ASE_NUM_CVERTEX);

			faceData = strstr((char *)aseBuffer,faceId);
			gosASSERT(faceData != NULL);

			sprintf(faceId,"%s %d",ASE_MESH_VERTCOL_ID,(long)cVFaces.y);

			faceData = strstr(faceData,faceId);
			gosASSERT(faceData != NULL);

			faceData += strlen(faceId);
				
			GetNumberData(faceData,numberData);
			red = (float)atof(numberData);

			faceData += strlen(numberData)+1;
				
			GetNumberData(faceData,numberData);
			green = (float)atof(numberData);

			faceData += strlen(numberData)+1;

			GetNumberData(faceData,numberData);
			blue = (float)atof(numberData);

			redC = red * 0xff;
			greenC = green * 0xff;
			blueC = blue * 0xff;

			listOfTypeVertices[listOfTypeTriangles[i].Vertices[1]].aRGBLight = (0xff << 24) + (redC << 16) + (greenC << 8) + blueC;

			//Load up the VERTCOL2
			sprintf(faceId,"%s",ASE_NUM_CVERTEX);

			faceData = strstr((char *)aseBuffer,faceId);
			gosASSERT(faceData != NULL);

			sprintf(faceId,"%s %d",ASE_MESH_VERTCOL_ID,(long)cVFaces.z);

			faceData = strstr(faceData,faceId);
			gosASSERT(faceData != NULL);

			faceData += strlen(faceId);
				
			GetNumberData(faceData,numberData);
			red = (float)atof(numberData);

			faceData += strlen(numberData)+1;
				
			GetNumberData(faceData,numberData);
			green = (float)atof(numberData);

			faceData += strlen(numberData)+1;

			GetNumberData(faceData,numberData);
			blue = (float)atof(numberData);

			redC = red * 0xff;
			greenC = green * 0xff;
			blueC = blue * 0xff;

			listOfTypeVertices[listOfTypeTriangles[i].Vertices[2]].aRGBLight = (0xff << 24) + (redC << 16) + (greenC << 8) + blueC;
		}
		else
		{
			listOfTypeVertices[listOfTypeTriangles[i].Vertices[0]].aRGBLight = 
			listOfTypeVertices[listOfTypeTriangles[i].Vertices[1]].aRGBLight = 
			listOfTypeVertices[listOfTypeTriangles[i].Vertices[2]].aRGBLight = 0xff000000;
		}

		//------------------------------------------------------------

		//------------------------------------------------------------
		// Dig out the face Normal.
		sprintf(faceId,"%s %d",ASE_FACE_NORMAL_ID,i);
		faceData = strstr((char *)aseBuffer,faceId);
		gosASSERT(faceData != NULL);		

		faceData += strlen(faceId);
			
		GetNumberData(faceData,numberData);
		listOfTypeTriangles[i].faceNormal.x = -(float)atof(numberData);
		faceData += strlen(numberData)+1;
			
		GetNumberData(faceData,numberData);
		listOfTypeTriangles[i].faceNormal.z = (float)atof(numberData);
		faceData += strlen(numberData)+1;

		GetNumberData(faceData,numberData);
		listOfTypeTriangles[i].faceNormal.y = (float)atof(numberData);

		//------------------------------------------------------------
		char vertexID[256];
		sprintf(vertexID,"%s",ASE_VERTEX_NORMAL_ID);
		char *vertexData = strstr((char *)faceData,vertexID);
		GetNumberData(vertexData,numberData);
		long vertexNum = atol(numberData);
		vertexNormalCount[vertexNum]++;

		vertexData += strlen(vertexID)+strlen(numberData)+1;

			GetNumberData(vertexData,numberData);
			vertexData += strlen(numberData)+1;
			listOfTypeVertices[vertexNum].normal.x += -(float)atof(numberData);

			GetNumberData(vertexData,numberData);
			vertexData += strlen(numberData)+1;
			listOfTypeVertices[vertexNum].normal.z += (float)atof(numberData);

			GetNumberData(vertexData,numberData);
			listOfTypeVertices[vertexNum].normal.y += (float)atof(numberData);

		vertexData = strstr((char *)vertexData,vertexID);
		GetNumberData(vertexData,numberData);
		vertexNum = atol(numberData);
		vertexNormalCount[vertexNum]++;

		vertexData += strlen(vertexID)+strlen(numberData)+1;

			GetNumberData(vertexData,numberData);
			vertexData += strlen(numberData)+1;
			listOfTypeVertices[vertexNum].normal.x += -(float)atof(numberData);

			GetNumberData(vertexData,numberData);
			vertexData += strlen(numberData)+1;
			listOfTypeVertices[vertexNum].normal.z += (float)atof(numberData);

			GetNumberData(vertexData,numberData);
			listOfTypeVertices[vertexNum].normal.y += (float)atof(numberData);

		vertexData = strstr((char *)vertexData,vertexID);
		GetNumberData(vertexData,numberData);
		vertexNum = atol(numberData);
		vertexNormalCount[vertexNum]++;

		vertexData += strlen(vertexID)+strlen(numberData)+1;

			GetNumberData(vertexData,numberData);
			vertexData += strlen(numberData)+1;
			listOfTypeVertices[vertexNum].normal.x += -(float)atof(numberData);

			GetNumberData(vertexData,numberData);
			vertexData += strlen(numberData)+1;
			listOfTypeVertices[vertexNum].normal.z += (float)atof(numberData);

			GetNumberData(vertexData,numberData);
			listOfTypeVertices[vertexNum].normal.y += (float)atof(numberData);

		//------------------------------------------------------------
	}

	//-----------------------------------------------
	// Must average together the vertexNormals
	for (i=0;i<numTypeVertices;i++)
	{
		if (vertexNormalCount[i] == 0)
		{
			listOfTypeVertices[i].normal.x = 0.0f;
			listOfTypeVertices[i].normal.y = 0.0f;
			listOfTypeVertices[i].normal.z = 0.0f;
		}
		else
		{
			listOfTypeVertices[i].normal.x /= float(vertexNormalCount[i]);
			listOfTypeVertices[i].normal.y /= float(vertexNormalCount[i]);
			listOfTypeVertices[i].normal.z /= float(vertexNormalCount[i]);

			if (listOfTypeVertices[i].normal.GetLength() > Stuff::SMALL)
				listOfTypeVertices[i].normal.Normalize(listOfTypeVertices[i].normal);
		}
	}

	TG_Shape::tglHeap->Free(vertexNormalCount);
	return(0);
}	

//-------------------------------------------------------------------------------
//Function returns 0 if OK.  -1 if file not found or file not ASE Format.		
//This function loads the ASE file into the TG_Triangle and TG_Vertex lists.
//It allocates memory.  These are straight tglHeap->Mallocs at present.
//
// NOTE: Only takes the first GEOMOBJECT from the ASE file.  Multi-object
// Files will require user intervention to parse!!
long TG_TypeShape::LoadTGShapeFromASE (char *fileName)
{
	long i;
	//-----------------------------------------------------
	// Fully loads and parses an ASE File.  These files are
	// output by 3D Studio MAX's ASCII Scene Exporter.

	//------------------------------------------
	// Check if exists by getting size
	long aseFileSize = gos_FileSize(fileName);
	gosASSERT(aseFileSize > 0);

	//---------------------------------------
	// Create Buffer to read entire file into
	BYTE *aseContents = (BYTE *)TG_Shape::tglHeap->Malloc(aseFileSize);
	gosASSERT(aseContents != NULL);

	//---------------------------------------
	// Open File and read it and close it
	HGOSFILE aseFile;
	gos_OpenFile(&aseFile,fileName,READONLY);

#ifdef _DEBUG
	long dataRead = 
#endif
		gos_ReadFile(aseFile,aseContents,aseFileSize);
	gosASSERT(dataRead == aseFileSize);

	gos_CloseFile(aseFile);

	//----------------------------------------
	// Check for valid ASE Header data.
	gosASSERT(strnicmp(ASE_HEADER,(char *)aseContents,strlen(ASE_HEADER)) == 0);

	//---------------------------------------
	// Find the number of Textures
	char textureId[256];
	char *textureData;

	char numberData[256];
	sprintf(textureId,ASE_MATERIAL_COUNT);
	textureData = strstr((char *)aseContents,textureId);
	if (!textureData)
		return(-1);

	textureData += strlen(ASE_MATERIAL_COUNT)+1;
	GetNumberData(textureData,numberData);
	long numMaterials = atol(numberData);

	numTextures = 0;
	unsigned char *aseBuffer = aseContents;
	for (long nmt=0;nmt<numMaterials;nmt++)
	{
		sprintf(textureId,ASE_MATERIAL_CLASS);
		textureData = strstr((char *)aseBuffer,textureId);
		if (!textureData)
			return(-1);

		textureData += strlen(ASE_MATERIAL_CLASS)+1;
		GetWordData(textureData,numberData);

		if (strstr(numberData,"Standard"))
		{
			numTextures++;
		}
		else if (strstr(numberData,"Multi"))
		{
			//----------------------------------------------------
			//NOT SUPPORTED YET!  Multiple Textures per building!
			// Support NOW!  Must have trees!
			textureData = strstr(textureData,ASE_SUBMATERIAL_COUNT);
			gosASSERT(textureData != NULL);

			textureData += strlen(ASE_SUBMATERIAL_COUNT);
			GetNumberData(textureData,numberData);

			numTextures += atol(numberData);
		}

		aseBuffer = (unsigned char *)textureData;
	}

	listOfTextures = (TG_TinyTexturePtr)TG_Shape::tglHeap->Malloc(sizeof(TG_TinyTexture) * numTextures);
	gosASSERT(listOfTextures != NULL);

	memset(listOfTextures,0x0,sizeof(TG_TinyTexture) * numTextures);

	for (i=0;i<numTextures;i++)
	{
		//-------------------------------------------------------------------------------
		// Get and store texture Name.  Will need multiple for Multi-Sub if implemented
		// NEVER GET ACTUAL TEXTURES HERE.  ALWAYS HAPPENS IN MULTI!!!!!!!!!!
		// Just set 'em!
		listOfTextures[i].mcTextureNodeIndex = 0xffffffff;
		listOfTextures[i].gosTextureHandle = 0xffffffff;
		listOfTextures[i].textureAlpha = false;
	}

	//---------------------------------------------------
	// Calling from top will load just first GEOMOBJECT!
	long parseResult = ParseASEFile(aseContents,fileName);

	TG_Shape::tglHeap->Free(aseContents);
	aseContents = NULL;

	return(parseResult);
}	

//-------------------------------------------------------------------------------
//Need this so that Multi-Shapes can let each shape know texture info.
void TG_TypeShape::CreateListOfTextures (TG_TexturePtr list, DWORD numTxms)
{
	long i;
	numTextures = numTxms;
	if (numTextures)
	{
		listOfTextures = (TG_TinyTexturePtr)TG_Shape::tglHeap->Malloc(sizeof(TG_TinyTexture) * numTextures);
		gosASSERT(listOfTextures != NULL);
   
	   for (i=0;i<numTextures;i++)
	   {
		   //-------------------------------------------------------------------------------
		   // Get and store texture Name.  Will need multiple for Multi-Sub if implemented
		   // NEVER GET ACTUAL TEXTURES HERE.  ALWAYS HAPPENS IN MULTI!!!!!!!!!!
		   // Just set 'em!
		   listOfTextures[i].mcTextureNodeIndex = list[i].mcTextureNodeIndex;
		   listOfTextures[i].gosTextureHandle = list[i].gosTextureHandle; 
		   listOfTextures[i].textureAlpha = list[i].textureAlpha;
	   }
 	}
	else
		listOfTextures = NULL;
}	

//-------------------------------------------------------------------------------
//Function returns 0 if OK.  -1 if textureNum is out of range of numTextures.
//This function takes the gosTextureHandle passed in and assigns it to the
//textureNum entry of the listOfTextures;
long TG_TypeShape::SetTextureHandle (DWORD textureNum, DWORD gosTextureHandle)
{
	if (textureNum >= numTextures)
		return(-1);

	listOfTextures[textureNum].mcTextureNodeIndex = gosTextureHandle;
	listOfTextures[textureNum].gosTextureHandle = mcTextureManager->get_gosTextureHandle(gosTextureHandle);

	return(0);
}	

//-------------------------------------------------------------------------------
//Function returns 0 if OK.  -1 if textureNum is out of range of numTextures.
//This function takes the gosTextureHandle passed in and assigns it to the
//textureNum entry of the listOfTextures;
long TG_TypeShape::SetTextureAlpha (DWORD textureNum, bool alphaFlag)
{
	if (textureNum >= numTextures)
		return(-1);

	listOfTextures[textureNum].textureAlpha = alphaFlag;

	return(0);
}

//-------------------------------------------------------------------------------
// TG_Shape
void *TG_Shape::operator new (size_t mySize)
{
	void *result = TG_Shape::tglHeap->Malloc(mySize);
	return result;
}
 
//-------------------------------------------------------------------------------
 void TG_Shape::operator delete (void *us)
{
	TG_Shape::tglHeap->Free(us);
}

//-------------------------------------------------------------------------------
//This function sets up the camera Matrices for this TG_Shape to transform
//itself with.  These matrices are static and only need to be set once per
//render pass if the camera does not change for that pass.
void TG_Shape::SetCameraMatrices (Stuff::LinearMatrix4D *camOrigin, Stuff::Matrix4D *camToClip)
{
	cameraOrigin = camOrigin;
	cameraToClip = camToClip;

	worldToCamera.Invert(*cameraOrigin);

	worldToClip.Multiply(worldToCamera, *cameraToClip);
}	

//-------------------------------------------------------------------------------
void TG_Shape::SetFog (DWORD fRGB, float fStart, float fFull)
{
	fogColor = fRGB;
	fogStart = fStart;
	fogFull = fFull;

	if (fogStart == fogFull)
	{
		fogStart = -100000.0f;
		fogFull = -99999.0f;
	}
}	

//-------------------------------------------------------------------------------
void TG_Shape::SetViewport (float mulX, float mulY, float addX, float addY)
{
	viewMulX = mulX;
	viewMulY = mulY;
	viewAddX = addX;
	viewAddY = addY;
}	

//-------------------------------------------------------------------------------
//This function sets the list of lights used by the TransformShape function
//to light the shape.
//Function returns 0 if lightList entries are all OK.  -1 otherwise.
//
long TG_Shape::SetLightList (TG_LightPtr *lightList, DWORD nLights)
{
	if (lightList)
	{
		listOfLights = lightList;
		numLights = nLights;
	}
	else
	{
		listOfLights = NULL;
		numLights = 0;
	}

	return 0;
}

long TG_Shape::SetParallelLightList (TG_LightPtr *lightList, DWORD nLights,
	long colorIndex, DWORD color, long inactiveIndex)
{
	const DWORD count = nLights < MAX_LIGHTS_IN_WORLD ? nLights : MAX_LIGHTS_IN_WORLD;
	for (DWORD i = 0; i < count; ++i)
	{
		if (lightList && lightList[i])
		{
			MC2_ParallelLights[i] = *lightList[i];
			if ((long)i == colorIndex)
				MC2_ParallelLights[i].SetaRGB(color);
			if ((long)i == inactiveIndex)
				MC2_ParallelLights[i].active = false;
			MC2_ParallelLightPtrs[i] = &MC2_ParallelLights[i];
		}
		else
			MC2_ParallelLightPtrs[i] = NULL;
	}
	listOfLights = MC2_ParallelLightPtrs;
	numLights = count;
	return 0;
}

//-------------------------------------------------------------------------------
//This function sets the fog values for the shape.  Straight fog right now.
void TG_Shape::SetFogRGB (DWORD fRGB)
{
	fogRGB = fRGB;
}	

//-------------------------------------------------------------------------------
void TG_TypeShape::movePosRelativeCenterNode (void)
{
	long i;
	for (i=0;i<numTypeVertices;i++)
	{
		listOfTypeVertices[i].position -= nodeCenter;
	}
}

extern thread_local float yawRotation;  // PORT (M8-B17): see msl.cpp
//-------------------------------------------------------------------------------
//This function does exactly what TranformShape does EXCEPT that the shapeToClip,
//Lighting and backface matrices have been calculated in the step above this one.
//This saves enormous processor cycles when matrices are the same and transforms
//Can just be run from the same matrices without recalcing them!
//Function returns -1 if all vertex screen positions are off screen.
//Function returns 0 if any one vertex screen position is off screen.
//Function returns 1 is all vertex screen positions are on screen.
// NOTE:  THIS IS NOT A RIGOROUS CLIP!!!!!!!!!
#define MAX_FOG_ELEVATION			100.0f
#define MAX_FOG_ELEVATION_INV		.01f
#define TGL_WINDOW_THRESHOLD		0.75f

// PORT (M8-B15): THE SILENT EARLY RETURN. When any per-frame TGL pool is
// exhausted the shape returns here having transformed nothing, so it draws
// zero faces while every visibility check upstream still calls it visible.
// Nothing reported it. This counts it.
// PORT (M8-B32): THE CENSUS NAMED THE WRONG POOLS, AND ITS COUNTERS WERE RACED.
//
// The old line always printed "(pools: 96000 verts / 64000 tris / 160000
// faces)" -- a fixed string naming the MAIN-thread pools. But every terrain
// object, which is every tree, bush, rock and building prop, is transformed by
// the parallel object workers (objmgr.cpp), and those draw from a SEPARATE and
// much smaller set built by TG_PrepareParallelPools. So a run could report peak
// demand of 85,840 against "96,000" and look like it had headroom while the
// pool that actually refused the work held 12,000. That is exactly the pairing
// this project has been bitten by before: a healthy-looking number measured
// against the wrong denominator.
//
// The counters were also plain longs written from four threads at once, and
// they did not even sum consistently -- one session reported vertices=1,228,547
// against a TOTAL of 1,228,398, which is arithmetically impossible for counters
// that are each incremented at most once per drop. Atomics, and demand
// accumulated thread_local so no two threads share a running total.
//
// Slot 0 is the main-thread pools; slot i+1 is object worker i. The main thread
// never calls TG_SelectParallelPool, so it runs its own share of the parallel
// object pass out of the slot-0 pools.
static const long MC2_POOL_SLOTS = 9;		// main + up to MC2_MAX_OBJECT_WORKERS

static std::atomic<long> MC2_PoolFail[MC2_POOL_SLOTS][6];
static std::atomic<long> MC2_PoolFailTotal(0);
static std::atomic<long> MC2_PoolPeakVerts[MC2_POOL_SLOTS];
static std::atomic<long> MC2_PoolPeakTris[MC2_POOL_SLOTS];
static long MC2_PoolUsedVerts[MC2_POOL_SLOTS] = {0};
static long MC2_PoolUsedTris[MC2_POOL_SLOTS] = {0};
static long MC2_PoolCapVerts[MC2_POOL_SLOTS] = {0};
static long MC2_PoolCapTris[MC2_POOL_SLOTS] = {0};
static long MC2_PoolCapFaces[MC2_POOL_SLOTS] = {0};
static bool MC2_PoolOn = false, MC2_PoolChecked = false;
static bool MC2_PoolPerTurn = false;

// Peak DEMAND per turn per slot, so the pools can be sized on a measurement
// instead of a guessed multiplier. Counts what every shape asked for, including
// the ones that were refused.
static thread_local long MC2_PoolWantVerts = 0, MC2_PoolWantTris = 0;
static thread_local long MC2_PoolWantTurn = -1;

static void MC2_PoolSummary (void);

void MC2_PoolNoteCapacity (long slot, long verts, long tris, long faces)
{
	if (slot < 0 || slot >= MC2_POOL_SLOTS)
		return;
	MC2_PoolCapVerts[slot] = verts;
	MC2_PoolCapTris[slot] = tris;
	MC2_PoolCapFaces[slot] = faces;
}

void MC2_PoolNotePeak (long slot, unsigned long verts, unsigned long tris)
{
	// The pool's OWN high-water mark, which is the number that settles an
	// argument the demand estimate below can only open: demand counts what was
	// asked for including what was refused, this counts what the pool held.
	if (slot < 0 || slot >= MC2_POOL_SLOTS)
		return;
	if ((long)verts > MC2_PoolUsedVerts[slot]) MC2_PoolUsedVerts[slot] = (long)verts;
	if ((long)tris > MC2_PoolUsedTris[slot]) MC2_PoolUsedTris[slot] = (long)tris;
}

void MC2_PoolFrameReset (void)
{
	// Arm HERE, not on the first starvation. Arming on failure meant a run with
	// no starvation printed nothing at all, so success and a broken census
	// looked identical. This runs every frame on the main thread, which is also
	// why the worker threads must never touch getenv or the arming flag.
	if (!MC2_PoolChecked)
	{
		MC2_PoolChecked = true;
		const char* e = getenv("MC2_POOL_CENSUS");
		MC2_PoolOn = e && *e && strcmp(e, "0") != 0;
		MC2_PoolPerTurn = MC2_PoolOn && atol(e) >= 2;
		if (MC2_PoolOn) atexit(MC2_PoolSummary);
	}
	// Called from mission.cpp between colorPool->reset() and vertexPool->reset(),
	// so these two still hold the frame that just finished.
	if (MC2_PoolOn && vertexPool && trianglePool)
		MC2_PoolNotePeak(0, vertexPool->getPeak(), trianglePool->getPeak());

	// MC2_POOL_CENSUS=2 reports the drops turn by turn. A shutdown total says a
	// run lost shapes; it does NOT say whether the ONE frame a headless capture
	// kept was one of the frames that lost them, and a captured frame that
	// looks correct proves nothing until that is known. This answers it.
	if (MC2_PoolPerTurn)
	{
		static long lastReported = 0;
		const long now = MC2_PoolFailTotal.load(std::memory_order_relaxed);
		if (now != lastReported)
		{
			printf("[pool-turn] turn=%ld dropped=%ld (total %ld)\n",
			       turn, now - lastReported, now);
			lastReported = now;
		}
	}
}

static void MC2_PoolRaise (std::atomic<long>& peak, long value)
{
	long seen = peak.load(std::memory_order_relaxed);
	while (value > seen &&
	       !peak.compare_exchange_weak(seen, value, std::memory_order_relaxed))
	{ }
}

static void MC2_PoolWant (long nv, long nt)
{
	// Roll the window on the TURN, not on a pool-reset call site. The first
	// version hooked ONE of the several reset sites and the owner's session took
	// a different one, so demand accumulated across frames and reported a peak
	// of 85,875 triangles against a 64,000 pool WITH ZERO STARVATION -- an
	// impossible pair, and that is what gave the instrument away. `turn`
	// advances once per game update and cannot fall out of step with it.
	if (turn != MC2_PoolWantTurn)
	{
		const long slot = (MC2_PoolSlot >= 0 && MC2_PoolSlot < MC2_POOL_SLOTS) ? MC2_PoolSlot : 0;
		MC2_PoolWantTurn = turn;
		MC2_PoolRaise(MC2_PoolPeakVerts[slot], MC2_PoolWantVerts);
		MC2_PoolRaise(MC2_PoolPeakTris[slot], MC2_PoolWantTris);
		MC2_PoolWantVerts = MC2_PoolWantTris = 0;
	}
	MC2_PoolWantVerts += nv;
	MC2_PoolWantTris  += nt;
}

static void MC2_PoolSummary (void)
{
	if (!MC2_PoolOn) return;
	printf("[pool] shapes dropped for want of pool memory = %ld\n",
	       MC2_PoolFailTotal.load(std::memory_order_relaxed));
	for (long slot = 0; slot < MC2_POOL_SLOTS; ++slot)
	{
		const long pv = MC2_PoolPeakVerts[slot].load(std::memory_order_relaxed);
		const long pt = MC2_PoolPeakTris[slot].load(std::memory_order_relaxed);
		long drops[6];
		long dropped = 0;
		for (long k = 0; k < 6; ++k)
		{
			drops[k] = MC2_PoolFail[slot][k].load(std::memory_order_relaxed);
			dropped += drops[k];
		}
		// A slot that was never selected has nothing to say. Printing it anyway
		// is how a pool that is allocated and never used stays invisible.
		if (!pv && !pt && !dropped)
			continue;
		if (slot == 0)
			printf("[pool] main     ");
		else
			printf("[pool] worker%ld  ", slot - 1);
		printf("wanted/turn verts=%6ld tris=%6ld\n", pv, pt);
		if (dropped)
			printf("[pool]         starved on vertices=%ld colors=%ld shadows=%ld triangles=%ld faces=%ld visShadows=%ld\n",
			       drops[0], drops[1], drops[2], drops[3], drops[4], drops[5]);
	}
	if (MC2_PoolCapVerts[0])
		printf("[pool] main          pool: high water verts=%6ld tris=%6ld of capacity verts=%6ld tris=%6ld faces=%6ld\n",
		       MC2_PoolUsedVerts[0], MC2_PoolUsedTris[0],
		       MC2_PoolCapVerts[0], MC2_PoolCapTris[0], MC2_PoolCapFaces[0]);
	// HIGH WATER OVERSHOOTS ITS OWN CAPACITY once a pool has starved, and that
	// is not a bug in the reading: retail raises the count before testing it and
	// never rolls a refused request back, so a starved pool goes on counting
	// what it could not hand out. Below capacity the number is exact.
	// EVERY worker shares one pool since M8-B32, so its line is printed once,
	// here, and NOT from inside the loop above. Slot 1 is worker 0 -- the share
	// the main thread runs itself out of the MAIN pools -- so it is always empty
	// and hanging the shared line off it printed nothing at all.
	if (MC2_PoolCapVerts[1])
		printf("[pool] shared worker pool: high water verts=%6ld tris=%6ld of capacity verts=%6ld tris=%6ld faces=%6ld\n",
		       MC2_PoolUsedVerts[1], MC2_PoolUsedTris[1],
		       MC2_PoolCapVerts[1], MC2_PoolCapTris[1], MC2_PoolCapFaces[1]);
	fflush(stdout);
}


static void MC2_PoolStarved (bool v, bool c, bool s, bool t, bool f, bool vs)
{
	// Armed on the main thread by MC2_PoolFrameReset. Worker threads must not
	// arm it themselves -- getenv and atexit from four threads at once is a
	// race, and this path runs from all of them.
	if (!MC2_PoolOn) return;
	const long slot = (MC2_PoolSlot >= 0 && MC2_PoolSlot < MC2_POOL_SLOTS) ? MC2_PoolSlot : 0;
	MC2_PoolFailTotal.fetch_add(1, std::memory_order_relaxed);
	if (v) MC2_PoolFail[slot][0].fetch_add(1, std::memory_order_relaxed);
	if (c) MC2_PoolFail[slot][1].fetch_add(1, std::memory_order_relaxed);
	if (s) MC2_PoolFail[slot][2].fetch_add(1, std::memory_order_relaxed);
	if (t) MC2_PoolFail[slot][3].fetch_add(1, std::memory_order_relaxed);
	if (f) MC2_PoolFail[slot][4].fetch_add(1, std::memory_order_relaxed);
	if (vs) MC2_PoolFail[slot][5].fetch_add(1, std::memory_order_relaxed);
}
//-----------------------------------------------------------------------------
// PORT (perf, 2026-08-22): split the SHAPE TRANSFORM the way the frame and
// Mission::update were split, because that pattern has worked twice.
//
// Fixing M8-B15 stopped shapes being silently dropped, so the game now does the
// transform work it always should have. Average frame rate did not move
// (134 -> 136 fps) but hitches doubled in frequency, and the owner wants 144
// WITHOUT losing draw distance or detail. Render time is only ~4 ms while logic
// is ~49 ms on a slow frame, so the target is CPU transform work, not the GPU.
//
// Two loops dominate this function: one over vertices (transform + per-vertex
// lighting) and one over triangles (backface + face lighting). This says which.
//
// SILENT unless MC2_XFORM_CENSUS is non-zero. Costs four clock reads per shape
// when armed, which is why it is not on by default.
//-----------------------------------------------------------------------------
static bool   MC2_XformOn = false, MC2_XformChecked = false;
static double MC2_XformTotalMs = 0.0, MC2_XformVertMs = 0.0, MC2_XformTriMs = 0.0;
static long   MC2_XformCalls = 0, MC2_XformVerts = 0, MC2_XformTris = 0;

static void MC2_XformSummary (void)
{
	if (!MC2_XformOn) return;
	printf("[xform] MultiTransformShape calls=%ld  total=%.0f ms\n",
	       MC2_XformCalls, MC2_XformTotalMs);
	printf("[xform]   vertex loop %.0f ms (%.1f%%) over %ld vertices\n",
	       MC2_XformVertMs,
	       MC2_XformTotalMs > 0.0 ? 100.0 * MC2_XformVertMs / MC2_XformTotalMs : 0.0,
	       MC2_XformVerts);
	printf("[xform]   triangle loop + rest %.0f ms (%.1f%%) over %ld triangles\n",
	       MC2_XformTotalMs - MC2_XformVertMs,
	       MC2_XformTotalMs > 0.0 ? 100.0 * (MC2_XformTotalMs - MC2_XformVertMs) / MC2_XformTotalMs : 0.0,
	       MC2_XformTris);
	fflush(stdout);
}

static inline bool MC2_XformCensus (void)
{
	if (!MC2_XformChecked)
	{
		MC2_XformChecked = true;
		const char* e = getenv("MC2_XFORM_CENSUS");
		MC2_XformOn = e && *e && strcmp(e, "0") != 0;
		if (MC2_XformOn) atexit(MC2_XformSummary);
	}
	return MC2_XformOn;
}

typedef std::chrono::steady_clock MC2Clock;
static inline double MC2_MsSince (const MC2Clock::time_point& t0)
{
	return std::chrono::duration<double, std::milli>(MC2Clock::now() - t0).count();
}
struct MC2XformGuard
{
	bool on;
	MC2Clock::time_point t0;
	MC2XformGuard (bool armed) : on(armed)
	{
		if (on) { t0 = MC2Clock::now(); MC2_XformCalls++; }
	}
	~MC2XformGuard (void)
	{
		if (on) MC2_XformTotalMs += MC2_MsSince(t0);
	}
};
long TG_Shape::MultiTransformShape (Stuff::Matrix4D *shapeToClip, Stuff::Point3D *backFacePoint, TG_ShapeRecPtr parentNode, bool isHudElement, BYTE alphaValue, bool isClamped)
{
	long j;
	long i;
	if (!numVertices)		//WE are the root Shape which may have no shape or a helper shape which defintely has no shape!
		return(1);

	const bool mc2Xf = MC2_XformCensus();
	MC2XformGuard mc2XfGuard(mc2Xf);
		
	//-------------------------------------------------
	// Transform entire list of vertices.
	// shapeOrigin is ShapeToWorld.
	// invShapeOrigin is WorldToShape.
	bool oneOff = false;
	bool oneOn = false;

	bool isNight = eye->getIsNight();
	float nightFactor = eye->getNightFactor();

	if (lightsOut)
	{
		isNight = false;
		nightFactor = 0.0f;
	}
	
	if (isSpotlight && !isNight)
	{
		listOfVertices = NULL;		//Mark this as untransformed!!
		return(1);
	}
		
	TG_TypeShapePtr theShape = (TG_TypeShapePtr)myType;
	
	//At this point, we know we are going to process this shape,
	// Get memory for its components from the pools!
	MC2_PoolWant((long)numVertices, (long)numTriangles);
	listOfVertices = vertexPool->getVerticesFromPool(numVertices);
	listOfColors = colorPool->getColorsFromPool(numVertices);
	
	listOfShadowTVertices = shadowPool->getShadowsFromPool(numVertices);

	listOfTriangles = trianglePool->getTrianglesFromPool(numTriangles);

	listOfVisibleFaces = facePool->getFacesFromPool(numTriangles);
	listOfVisibleShadows = facePool->getFacesFromPool(numTriangles);

	if (!listOfVertices ||
		!listOfColors ||
		!listOfShadowTVertices ||
		!listOfTriangles ||
		!listOfVisibleFaces ||
		!listOfVisibleShadows)
	{
		MC2_PoolStarved(!listOfVertices, !listOfColors, !listOfShadowTVertices,
		                !listOfTriangles, !listOfVisibleFaces, !listOfVisibleShadows);
		return(1);
	}

	lastTurnTransformed = turn;

	MC2Clock::time_point mc2XfV; if (mc2Xf) mc2XfV = MC2Clock::now();
	for (j=0;j<numVertices;j++)
	{
		Stuff::Point3D pos = theShape->listOfTypeVertices[j].position;
		if (shapeScalar > 0.0f)
			pos *= shapeScalar;
			
		Stuff::Vector4D xformCoords;
		Stuff::Vector4D screen;

		xformCoords.Multiply(pos,*shapeToClip);

		if (eye->usePerspective)
		{
			//---------------------------------------
			// Perspective Transform
			float rhw = 1.0f;
			if (xformCoords.w != 0.0f)
				rhw = 1.0f / xformCoords.w;
			
			screen.x = (xformCoords.x * rhw) * viewMulX + viewAddX;
			screen.y = (xformCoords.y * rhw) * viewMulY + viewAddY;
			screen.z = (xformCoords.z * rhw);
			screen.w = fabs(rhw);
		}
		else
		{
			//---------------------------------------
			// Parallel Transform	
			screen.x = (1.0f - xformCoords.x) * viewMulX + viewAddX;
			screen.y = (1.0f - xformCoords.y) * viewMulY + viewAddY;
			screen.z = xformCoords.z;
			screen.w = 0.000001f;
		}

		if ((screen.x < 0) || (screen.y < 0) || (screen.x >= viewMulX) || (screen.y >= viewMulY))
			oneOff = TRUE;

		if ((screen.x >= 0) && (screen.y >= 0) && (screen.x < viewMulX) && (screen.y <= viewMulY))
			oneOn = TRUE;

		listOfVertices[j].x = screen.x;
		listOfVertices[j].y = screen.y;
		listOfVertices[j].z = screen.z;
		listOfVertices[j].rhw = screen.w;
		listOfVertices[j].frgb = fogRGB;
		memset(&listOfColors[j],0,sizeof(listOfColors[j]));

		//----------------------------------------------------
		// Lighting goes here.
		DWORD redFinal=0, greenFinal=0, blueFinal=0;
		DWORD redAmb = 0, greenAmb = 0, blueAmb = 0;
		DWORD redSpec=0, greenSpec=0, blueSpec=0;
		
		if (lighteningLevel > 0)
		{
			redSpec = blueSpec = greenSpec = lighteningLevel;
		}

		DWORD startVLight = theShape->listOfTypeVertices[j].aRGBLight;
   		if (startVLight == 0xffff00ff)			//Hot Pink -- Lit Windows -- ONLY at NIGHT
   		{
   			if (isNight)
   			{
   				redFinal = (theShape->hotPinkRGB>>16) & 0x000000ff;
   				greenFinal = (theShape->hotPinkRGB>>8) & 0x000000ff;
   				blueFinal = (theShape->hotPinkRGB) & 0x000000ff;
   			}
   			else if (nightFactor > Stuff::SMALL)
   			{
   				redFinal = (theShape->hotPinkRGB>>16) & 0x000000ff;
   				greenFinal = (theShape->hotPinkRGB>>8) & 0x000000ff;
   				blueFinal = (theShape->hotPinkRGB) & 0x000000ff;
   				
   				redFinal *= nightFactor;
   				blueFinal *= nightFactor;
   				greenFinal *= nightFactor;
   			}
			else		//Its not night, paint windows dark grey
			{
				redFinal = 0x2f;
				greenFinal = 0x2f;
				blueFinal = 0x2f;
			}
   		}
   		else if (startVLight == 0xffffff00)		//Hot Yellow -- Outside Building Lights -- ONLY at NIGHT
   		{
   			if (nightFactor >= 0.75f)
   			{
   				if (isNight)
   				{
   					redFinal = (theShape->hotYellowRGB>>16) & 0x000000ff;
   					greenFinal = (theShape->hotYellowRGB>>8) & 0x000000ff;
   					blueFinal = (theShape->hotYellowRGB) & 0x000000ff;
   				}
   				else
   				{
   					redFinal = (theShape->hotYellowRGB>>16) & 0x000000ff;
   					greenFinal = (theShape->hotYellowRGB>>8) & 0x000000ff;
   					blueFinal = (theShape->hotYellowRGB) & 0x000000ff;
   					
   					redFinal *= nightFactor;
   					blueFinal *= nightFactor;
   					greenFinal *= nightFactor;
   				}
   			}
   		}
   		else if (startVLight == 0xff00ff00)		//Hot Green -- Building Base Lights -- ONLY at NIGHT
   		{
   			if (isNight)
   			{
   				redFinal = (theShape->hotGreenRGB>>16) & 0x000000ff;
   				greenFinal = (theShape->hotGreenRGB>>8) & 0x000000ff;
   				blueFinal = (theShape->hotGreenRGB) & 0x000000ff;
   			}
   			else if (nightFactor > Stuff::SMALL)
   			{
   				redFinal = (theShape->hotGreenRGB>>16) & 0x000000ff;
   				greenFinal = (theShape->hotGreenRGB>>8) & 0x000000ff;
   				blueFinal = (theShape->hotGreenRGB) & 0x000000ff;
   				
   				redFinal *= nightFactor;
   				blueFinal *= nightFactor;
   				greenFinal *= nightFactor;
   			}
   		}
   		else if (startVLight == 0xffff0000)		//Hot Red -- Blink this light.
   		{
   		
   		}
   		else if (startVLight == 0xff0000ff)		//Hot Blue -- Blink this light.
   		{
   		
   		}
   		else if (startVLight & 0x00ffffff)		//Some other kind of light, just add it in.
   		{
			if (!lightsOut)
			{
				redFinal = (startVLight>>16) & 0x000000ff;
				greenFinal = (startVLight>>8) & 0x000000ff;
				blueFinal = (startVLight) & 0x000000ff;
			}
   		}
		else if (isHudElement)
		{
			redFinal = blueFinal = greenFinal = 0xff;	//Just max out its light.
		}

		if (BaseVertexColor)
		{
			redFinal += ((BaseVertexColor>>16) & 0x000000ff);
			if (redFinal > 0xff)
				redFinal = 0xff;
				
			greenFinal += ((BaseVertexColor>>8) & 0x000000ff);
			if (greenFinal > 0xff)
				greenFinal = 0xff;
				
			blueFinal += (BaseVertexColor & 0x000000ff);
			if (blueFinal > 0xff)
				blueFinal = 0xff;
		}
			
		if (useVertexLighting && (Environment.Renderer != 3))
		{
			if (!isSpotlight && !isWindow)
			{
				for (i=0;i<numLights;i++)
				{
					if ((listOfLights[i] != NULL) && (listOfLights[i]->active))
					{
						DWORD startLight = listOfLights[i]->GetaRGB();
						switch (listOfLights[i]->lightType)
						{
							case TG_LIGHT_AMBIENT:
							{
								redAmb = ((startLight>>16) & 0x000000ff);
								greenAmb = ((startLight>>8) & 0x000000ff);
								blueAmb = ((startLight) & 0x000000ff);
							}
							break;
	
							case TG_LIGHT_INFINITE:
							{
								float cosine = lightDir[i].x * theShape->listOfTypeVertices[j].normal.x;
								cosine += lightDir[i].y * theShape->listOfTypeVertices[j].normal.y;
								cosine += lightDir[i].z * theShape->listOfTypeVertices[j].normal.z;

								if (cosine < 0.0f)
								{
									float cos = fabs(cosine);
									float red = float((startLight>>16) & 0x000000ff) * cos;
									float green = float((startLight>>8) & 0x000000ff) * cos;
									float blue = float((startLight) & 0x000000ff) * cos;

									redFinal += float2long(red);
									greenFinal += float2long(green);
									blueFinal += float2long(blue);

									/*
									//-----------------------
									// Run Specular next.
									// Mirror the Light vector.
									// Then DOT with Camera vector.
									// Mul Result by itself 4 times.
									// Mul that result by shinyness.
									// Multiply THAT through color and apply!
									float mirrorScalar = cosine * 2.0f;
									Stuff::Vector3D MirrorVector(listOfVertices[i].normal);
									MirrorVector *= mirrorScalar;
									MirrorVector.Subtract(MirrorVector,lightDir[i]);
									MirrorVector.Normalize(MirrorVector);

									Stuff::Vector3D SpecPoint;
									if (turn > 3)
										SpecPoint.Normalize(*backFacePoint);
									float specular = SpecPoint.x * MirrorVector.x;
									specular += SpecPoint.y * MirrorVector.y;
									specular += SpecPoint.z * MirrorVector.z;

									if (specular < 0.0f)
									{
										//specular = fabs(specular);
										specular *= specular;
										specular *= specular;
										specular *= specular;
										specular *= specular;

										specular *= 1.0f;	
										// For now, a shinyness of 1.0f.  Perfect reflection!
										// Multiplies will make specular positive!
										float red = float((startLight>>16) & 0x000000ff) * specular;
										float green = float((startLight>>8) & 0x000000ff) * specular;
										float blue = float((startLight) & 0x000000ff) * specular;

										redSpec += (DWORD)red;
										greenSpec += (DWORD)green;
										blueSpec += (DWORD)blue;
									}
									*/
								}
							}
							break;
	
							case TG_LIGHT_INFINITEWITHFALLOFF:
							{
								Stuff::Point3D vertexToLight;
								vertexToLight = lightToShape[i];
								vertexToLight -= theShape->listOfTypeVertices[j].position;
	
								float length = vertexToLight.GetApproximateLength();
	
								float falloff = 1.0f;
	
								float red,green,blue;
	
								if (listOfLights[i]->GetFalloff(length, falloff))
								{
									float cosine = -(lightDir[i] * (theShape->listOfTypeVertices[j].normal));
	
									red = float((startLight>>16) & 0x000000ff) * falloff;
									green = float((startLight>>8) & 0x000000ff) * falloff;
									blue = float((startLight) & 0x000000ff) * falloff;
	
									red *= cosine;
									green *= cosine;
									blue *= cosine;
	
									redFinal += (DWORD)red;
									greenFinal += (DWORD)green;
									blueFinal += (DWORD)blue;
								}
							}
							break;
	
							case TG_LIGHT_POINT:
							{
								Stuff::Point3D vertexToLight;
								vertexToLight = lightDir[i];
								float length = vertexToLight.GetApproximateLength();
	
								if (length > Stuff::SMALL)
								{	
									vertexToLight.Normalize(vertexToLight);
		
									float falloff = 1.0f;
		
									if (listOfLights[i]->GetFalloff(length, falloff))
									{
										float cosine = vertexToLight * (theShape->listOfTypeVertices[j].normal);
		
										float red,green,blue;
		
										if (cosine < 0.0f)
										{
											cosine = fabs(cosine);
											red = float((startLight>>16) & 0x000000ff) * falloff;
											green = float((startLight>>8) & 0x000000ff) * falloff;
											blue = float((startLight) & 0x000000ff) * falloff;
		
											red *= cosine;
											green *= cosine;
											blue *= cosine;
		
											redSpec += (DWORD)red;
											greenSpec += (DWORD)green;
											blueSpec += (DWORD)blue;
										}
									}
								}
								else
								{
									//Object is in center of light.  BLOW IT OUT!
									float cosine = 1.0f;
	
									float red,green,blue;
	
									if (cosine < 0.0f)
									{
										cosine = fabs(cosine);
										red = float((startLight>>16) & 0x000000ff);
										green = float((startLight>>8) & 0x000000ff);
										blue = float((startLight) & 0x000000ff);
	
										red *= cosine;
										green *= cosine;
										blue *= cosine;
	
										redSpec += (DWORD)red;
										greenSpec += (DWORD)green;
										blueSpec += (DWORD)blue;
									}
								}
							}
							break;
								  
							case TG_LIGHT_TERRAIN:
							{
								if (useShadows)
								{
									Stuff::Point3D vertexToLight;
									Stuff::Vector3D pos = theShape->listOfTypeVertices[j].position;
									RotateLight(pos,yawRotation);
									vertexToLight.Add(lightDir[i],pos);
									float length = vertexToLight.GetApproximateLength();
		
									if (length > Stuff::SMALL)
									{	
										float falloff = 1.0f;
										if (listOfLights[i]->GetFalloff(length, falloff))
										{
											float red,green,blue;
			
											red = float((startLight>>16) & 0x000000ff) * falloff;
											green = float((startLight>>8) & 0x000000ff) * falloff;
											blue = float((startLight) & 0x000000ff) * falloff;
			
											listOfColors[j].redSpec = (DWORD)red;
											listOfColors[j].greenSpec = (DWORD)green;
											listOfColors[j].blueSpec = (DWORD)blue;
										}
									}
									else
									{
										//Object is in center of light.  NOTHING HAPPENS WITH THIS KIND!!!
										//Light is already burned in!
									}
								}
							}
							break;
							
 							case TG_LIGHT_SPOT:
							{
								Stuff::Point3D vertexToLight;
								vertexToLight = lightDir[i];
								
								//-------------------------------------------------
								// Defines the actual spot of light on the ground
								float length = vertexToLight.GetApproximateLength();
	
								//-------------------------------------------------
								// Defines the REAL direction of the spot light.
								vertexToLight = spotDir[i];
								if (vertexToLight.GetApproximateLength() > Stuff::SMALL)
									vertexToLight.Normalize(vertexToLight);
								else
									length = 99999999999999.0f;
	
								float falloff = 1.0f;
	
								if (listOfLights[i]->GetFalloff(length, falloff))
								{
									float cosine = vertexToLight * (theShape->listOfTypeVertices[j].normal);
	
									float red,green,blue;
	
									if (cosine < 0.0f)
									{
										cosine = fabs(cosine);
										red = float((startLight>>16) & 0x000000ff) * falloff;
										green = float((startLight>>8) & 0x000000ff) * falloff;
										blue = float((startLight) & 0x000000ff) * falloff;
	
										red *= cosine;
										green *= cosine;
										blue *= cosine;
	
										redSpec += (DWORD)red;
										greenSpec += (DWORD)green;
										blueSpec += (DWORD)blue;
									}
								}
							}
							break;
						}
					}
				}
				
				redFinal += redAmb;
				blueFinal += blueAmb;
				greenFinal += greenAmb;
			}
			
			if (redFinal > 255)
				redFinal = 255;

			if (greenFinal > 255)
				greenFinal = 255;

			if (blueFinal > 255)
				blueFinal = 255;
					
			listOfVertices[j].argb = (0xff << 24) + (redFinal << 16) + (greenFinal << 8) + (blueFinal);

			// NEW
			// Add in the pre-calced or calced this frame terrain lights.
			// Terrain lights will only be made ACTIVE when day/night changes.
			// One frame of active is enough to store new values!
			redSpec += listOfColors[j].redSpec;
			greenSpec += listOfColors[j].greenSpec;
			blueSpec += listOfColors[j].blueSpec;
			
			if (redSpec > 255)
				redSpec = 255;

			if (greenSpec > 255)
				greenSpec = 255;

			if (blueSpec > 255)
				blueSpec = 255;
		}
		else
		{
			listOfVertices[j].argb = (0xff << 24) + (redAmb << 16) + (greenAmb << 8) + (blueAmb);
		}

		DWORD fogValue = (listOfVertices[j].frgb >> 24) & 0x000000ff;
		if (Environment.Renderer != 3)
		{
			if (useFog)
			{
				float relElevation = theShape->listOfTypeVertices[j].position.y;
				relElevation += theShape->relativeNodeCenter.y;

				TG_ShapeRecPtr curNode = parentNode;
				while (curNode && curNode->node)
				{
					relElevation += curNode->node->myType->GetRelativeNodeCenter().y;
					curNode = curNode->parentNode;
				}

				if (relElevation < MAX_FOG_ELEVATION)
				{
					float fogFactor = MAX_FOG_ELEVATION - relElevation;
					if (fogFactor >= 0.0)
					{
						fogFactor /= (MAX_FOG_ELEVATION);
						if (fogFactor <= 1.0)
						{
							fogFactor *= fogFactor;
							fogFactor = 1.0 - fogFactor;
						}
						else
						{
							fogFactor = 0.0;
						}

						fogValue += float2long((0x000000ff - fogValue) * fogFactor);
					}
				}
				else
				{
					fogValue = 0x000000ff;
				}

				listOfVertices[j].frgb = (fogValue << 24) + (redSpec << 16) + (greenSpec << 8) + (blueSpec);
			}
			else
			{
				listOfVertices[j].frgb = (0xff << 24) + (redSpec << 16) + (greenSpec << 8) + (blueSpec);
			}
		}
		else
		{
			listOfVertices[j].frgb = (0xff << 24) + (redSpec << 16) + (greenSpec << 8) + (blueSpec);
		}

		//-------------------
		// Distance FOG now.
		if (useFog && Camera::HazeFactor != 0.0f)
		{
			float fogFactor = 1.0 - Camera::HazeFactor;
			DWORD distFog = float2long(fogFactor * 255.0f);
			
			if (distFog < fogValue)
				fogValue = distFog;
				
			listOfVertices[j].frgb = (fogValue << 24) + (redSpec << 16) + (greenSpec << 8) + (blueSpec);
		}
		
		if (aRGBHighlight)
		{
			DWORD frgb = listOfVertices[j].frgb;

			DWORD fFinal = (frgb>>24) & 0x000000ff;
			DWORD rFinal = (frgb>>16) & 0x000000ff;
			DWORD gFinal = (frgb>>8) & 0x000000ff;
			DWORD bFinal = (frgb) & 0x000000ff;

			rFinal += (aRGBHighlight>>16) & 0x000000ff;
			if (rFinal > 255)
				rFinal = 255;

			gFinal += (aRGBHighlight>>8) & 0x000000ff;
			if (gFinal > 255)
				gFinal = 255;

			bFinal += (aRGBHighlight) & 0x000000ff;;
			if (bFinal > 255)
				bFinal = 255;

			frgb = (fFinal << 24) + (rFinal << 16) + (gFinal << 8) + (bFinal);
			listOfVertices[j].frgb = frgb;
		}
	}

	if (mc2Xf) { MC2_XformVertMs += MC2_MsSince(mc2XfV); MC2_XformVerts += (long)numVertices; }
	numVisibleFaces = 0;			//Reset Visible Faces

	TG_TypeTrianglePtr tri = &(theShape->listOfTypeTriangles[0]);
	for (j=0;j<numTriangles;j++,tri++)
	{
		//---------------------------------------
		// Mark backfacing those that are.
		float cosine = 0.0;
		cosine = (*backFacePoint * (tri->faceNormal));
		if ((cosine > 0.0f) || isSpotlight)
		{
			listOfVisibleFaces[numVisibleFaces] = j;
			numVisibleFaces++;

			DWORD redFinal = 0, greenFinal = 0, blueFinal = 0;
			DWORD redSpec=0, greenSpec=0, blueSpec=0;
			DWORD redAmb=0, greenAmb=0, blueAmb=0;
	
			if (useFaceLighting)
			{
				//--------------------------------------------
				// Flat Shade any face which are flat shaded.
				for (i=0;i<numLights;i++)
				{
					if (listOfLights[i] != NULL)
					{
						DWORD startVLight = listOfLights[i]->GetaRGB();
						switch (listOfLights[i]->lightType)
						{
							case TG_LIGHT_INFINITE:
							{
								if (lightDir[i].GetLength() > Stuff::SMALL)
									lightDir[i].Normalize(lightDir[i]);
		
								float cosine = -(lightDir[i] * (theShape->listOfTypeTriangles[j].faceNormal));
		
								if (cosine > 0.0f)
								{
									float red = float((startVLight>>16) & 0x000000ff) * cosine;
									float green = float((startVLight>>8) & 0x000000ff) * cosine;
									float blue = float((startVLight) & 0x000000ff) * cosine;
		
									redFinal += float2long(red);
									greenFinal += float2long(green);
									blueFinal += float2long(blue);
								}
							}
							break;
		
							case TG_LIGHT_INFINITEWITHFALLOFF:
							{
								if (lightDir[i].GetLength() > Stuff::SMALL)
									lightDir[i].Normalize(lightDir[i]);
		
								float cosine = -(lightDir[i] * (theShape->listOfTypeTriangles[j].faceNormal));
		
								Stuff::Point3D vertexToLight;
								vertexToLight = lightToShape[i];
								vertexToLight -= theShape->listOfTypeVertices[theShape->listOfTypeTriangles[j].Vertices[0]].position;
		
								float length = vertexToLight.GetApproximateLength();
		
								float falloff = 1.0f;
		
								float red,green,blue;
		
								if ((cosine > 0.0f) && listOfLights[i]->GetFalloff(length, falloff))
								{
									red = float((startVLight>>16) & 0x000000ff) * falloff;
									green = float((startVLight>>8) & 0x000000ff) * falloff;
									blue = float((startVLight) & 0x000000ff) * falloff;
		
									red *= cosine;
									green *= cosine;
									blue *= cosine;
		
									redFinal += (DWORD)red;
									greenFinal += (DWORD)green;
									blueFinal += (DWORD)blue;
								}
							}
							break;
		
							case TG_LIGHT_POINT:
							{
								Stuff::Point3D vertexToLight;
								vertexToLight = lightToShape[i];
								vertexToLight -= theShape->listOfTypeVertices[theShape->listOfTypeTriangles[j].Vertices[0]].position;
		
								float length = vertexToLight.GetApproximateLength();
		
								vertexToLight.Normalize(vertexToLight);
								float falloff = 1.0f;
		
								if (listOfLights[i]->GetFalloff(length, falloff))
								{
									float cosine = -(vertexToLight * (theShape->listOfTypeTriangles[j].faceNormal));
		
									float red,green,blue;
		
									if (cosine > 0.0f)
									{
										red = float((startVLight>>16) & 0x000000ff) * falloff;
										green = float((startVLight>>8) & 0x000000ff) * falloff;
										blue = float((startVLight) & 0x000000ff) * falloff;
		
										red *= cosine;
										green *= cosine;
										blue *= cosine;
		
										redSpec += (DWORD)red;
										greenSpec += (DWORD)green;
										blueSpec += (DWORD)blue;
									}
								}
							}
							break;
		
							case TG_LIGHT_AMBIENT:
							{
								redAmb = ((startVLight>>16) & 0x000000ff);
								greenAmb = ((startVLight>>8) & 0x000000ff);
								blueAmb = ((startVLight) & 0x000000ff);
							}
							break;
						}
					}
				}
		
				redFinal += redAmb;
				blueFinal += blueAmb;
				greenFinal += greenAmb;
				
				if (redFinal > 255)
					redFinal = 255;
	
				if (greenFinal > 255)
					greenFinal = 255;
	
				if (blueFinal > 255)
					blueFinal = 255;
					
				if (redSpec > 255)
					redSpec = 255;
	
				if (greenSpec > 255)
					greenSpec = 255;
	
				if (blueSpec > 255)
					blueSpec = 255;
			}
	
			for (i=0;i<3;i++ )
			{
				DWORD argb = listOfVertices[theShape->listOfTypeTriangles[j].Vertices[i]].argb;
				DWORD rFinal = (argb>>16) & 0x000000ff;
				DWORD gFinal = (argb>>8) & 0x000000ff; 
				DWORD bFinal = (argb) & 0x000000ff;    
	
				rFinal += redFinal;
				if (rFinal > 255)
					rFinal = 255;
	
				gFinal += greenFinal;
				if (gFinal > 255)
					gFinal = 255;
	
				bFinal += blueFinal;
				if (bFinal > 255)
					bFinal = 255;
	
				listOfTriangles[j].aRGBLight[i] = (alphaValue << 24) + (rFinal << 16) + (gFinal << 8) + (bFinal);
	
				DWORD frgb = listOfVertices[theShape->listOfTypeTriangles[j].Vertices[i]].frgb;
	
				DWORD fFinal = (frgb>>24) & 0x000000ff;
				rFinal = (frgb>>16) & 0x000000ff;
				gFinal = (frgb>>8) & 0x000000ff;
				bFinal = (frgb) & 0x000000ff;
	
				rFinal += (redSpec>>16) & 0x000000ff;
				if (rFinal > 255)
					rFinal = 255;
	
				gFinal += (greenSpec>>8) & 0x000000ff;
				if (gFinal > 255)
					gFinal = 255;
	
				bFinal += (blueSpec) & 0x000000ff;
				if (bFinal > 255)
					bFinal = 255;
	
				listOfTriangles[j].fRGBLight[i] = (fFinal << 24) + (rFinal << 16) + (gFinal << 8) + (bFinal);
			}
			
			DWORD addFlags = 0;
			if (isHudElement)
			{
				addFlags = MC2_ISCOMPASS;
			}
			
			if (isClamped)
			{
				addFlags = MC2_ISTERRAIN;
			}
			
			if (isSpotlight)
			{
				mcTextureManager->addTriangle(0xffffffff,MC2_ISSPOTLGT);
			}
			else if (isWindow)
			{
				mcTextureManager->addTriangle(0xffffffff,MC2_DRAWALPHA);
			}
			else
			{
				//--------------------------------------------------
				// Any sorting we need to do occurs in here as well!
				// So, find out what texture this face is using and mark that texture as needing 3 more vertices.
				if (theShape->listOfTextures[theShape->listOfTypeTriangles[j].localTextureHandle].textureAlpha || (alphaValue != 0xff))
				{
					mcTextureManager->addTriangle(theShape->listOfTextures[theShape->listOfTypeTriangles[j].localTextureHandle].mcTextureNodeIndex,MC2_DRAWALPHA | addFlags);
				}
				else
				{
					mcTextureManager->addTriangle(theShape->listOfTextures[theShape->listOfTypeTriangles[j].localTextureHandle].mcTextureNodeIndex,MC2_DRAWSOLID | addFlags);
				}
			}
		}
	}

	gosASSERT(oneOff || oneOn);

	if (oneOff && !oneOn)
	{
		return(-1);
	}
	else if (oneOff && oneOn)
	{
		return(0);
	}		
	else if (!oneOff && oneOn)
	{
		return(1);
	}

	return(-1);
}

#define TERRAIN_DEPTH_FUDGE		(0.000f)
//-------------------------------------------------------------------------------
//This function takes the current listOfVisibleFaces and draws them using
//gos_DrawTriangle.
void TG_Shape::Render (float forceZ, bool isHudElement, BYTE alphaValue, bool isClamped)
{
	long j;
	if (!renderTGLShapes)
		return;
		
	if (!numVertices)		//WE are the root Shape which may have no shape or a helper shape which defintely has no shape!
		return;
		
	if (!listOfVertices ||
		!listOfColors ||
		!listOfShadowTVertices ||
		!listOfTriangles ||
		!listOfVisibleFaces ||
		!listOfVisibleShadows ||
		(/*(lastTurnTransformed != (turn-1)) &&*/ (lastTurnTransformed != turn)))
		return;

	if (fogColor != 0xffffffff)
	{
		gos_SetRenderState( gos_State_Fog, (intptr_t)&(fogColor));
	}
	else
	{
		gos_SetRenderState( gos_State_Fog, 0);
	}

	TG_TypeShapePtr theShape = (TG_TypeShapePtr)myType;

	gos_SetRenderState( gos_State_AlphaTest, theShape->alphaTestOn);
	gos_SetRenderState( gos_State_Filter, theShape->filterOn ? gos_FilterBiLinear : gos_FilterNone);

	DWORD lastTextureUsed = 0xffffffff;
	
	bool isNight = eye->getIsNight();
	float nightFactor = eye->getNightFactor();
	
	if (lightsOut)
	{
		isNight = false;
		nightFactor = 0.0f;
	}

	if (isSpotlight && !isNight)
		return;
 	
 	for (j=0;j<numVisibleFaces;j++)
	{
		if (listOfVisibleFaces[j] < numTriangles)
		{
			TG_TypeTriangle triType = theShape->listOfTypeTriangles[listOfVisibleFaces[j]];
			TG_Triangle tri = listOfTriangles[listOfVisibleFaces[j]];

			gos_VERTEX gVertex[3];

			gVertex[0] = listOfVertices[triType.Vertices[0]];
			gVertex[1] = listOfVertices[triType.Vertices[1]];
			gVertex[2] = listOfVertices[triType.Vertices[2]];

			gVertex[0].u		= triType.uvdata.u0;
			gVertex[0].v		= triType.uvdata.v0;

			gVertex[0].argb		= tri.aRGBLight[0];

			gVertex[1].u		= triType.uvdata.u1;
			gVertex[1].v		= triType.uvdata.v1;

			gVertex[1].argb		= tri.aRGBLight[1];

			gVertex[2].u		= triType.uvdata.u2;
			gVertex[2].v		= triType.uvdata.v2;

			gVertex[2].argb		= tri.aRGBLight[2];

			DWORD addFlags = 0;
			if (isHudElement)		//We are a HUD Element like the compass.  Mark us as such.
			{
				addFlags = MC2_ISCOMPASS;
			}
			
			if (isClamped)
			{
				addFlags = MC2_ISTERRAIN;
			}

			if (drawOldWay)
			{
			
				if (theShape->listOfTextures && 
					(theShape->listOfTextures[triType.localTextureHandle].gosTextureHandle != 0xffffffff))
				{
					if (theShape->listOfTextures[triType.localTextureHandle].gosTextureHandle != lastTextureUsed)
					{
						gos_SetRenderState( gos_State_Texture, theShape->listOfTextures[triType.localTextureHandle].gosTextureHandle );
						if (theShape->listOfTextures[triType.localTextureHandle].textureAlpha)
						{
							gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
							gos_SetRenderState( gos_State_AlphaTest, true);
						}
						
						lastTextureUsed = theShape->listOfTextures[triType.localTextureHandle].gosTextureHandle;
					}
				}
				else
					gos_SetRenderState( gos_State_Texture, 0 );
	
				if ((gVertex[0].z >= 0.0f) &&
					(gVertex[0].z < 1.0f) &&
					(gVertex[1].z >= 0.0f) &&  
					(gVertex[1].z < 1.0f) && 
					(gVertex[2].z >= 0.0f) &&  
					(gVertex[2].z < 1.0f))
				{
					//-----------------------------------------------------------------------------
					// Reject Any triangle which has vertices off screeen in software for now.
					// Do real cliping in geometry layer for software and hardware that needs it!
					gos_DrawTriangles(gVertex, 3);
				}
			}
			else
			{
				if ((gVertex[0].z >= 0.0f) &&
					(gVertex[0].z < 1.0f) &&
					(gVertex[1].z >= 0.0f) &&  
					(gVertex[1].z < 1.0f) && 
					(gVertex[2].z >= 0.0f) &&  
					(gVertex[2].z < 1.0f))
				{
					if ((forceZ >= 0.0f) && (forceZ < 1.0f))
					{
						gVertex[0].z	= forceZ;
						gVertex[1].z	= forceZ;
						gVertex[2].z	= forceZ;
					}
 				
					//Can make this a flag to optimize!
					if (isSpotlight)
					{
						mcTextureManager->addVertices(0xffffffff,gVertex,MC2_ISSPOTLGT);
					}
					else if (isWindow)
					{
						mcTextureManager->addVertices(0xffffffff,gVertex,MC2_DRAWALPHA);
					}
					else
					{
						//--------------------------------------------------
						// Any sorting we need to do occurs in here as well!
						// So, find out what texture this face is using and mark that texture as needing 3 more vertices.
						if (theShape->listOfTextures[triType.localTextureHandle].textureAlpha || (alphaValue != 0xff))
						{
							mcTextureManager->addVertices(theShape->listOfTextures[triType.localTextureHandle].mcTextureNodeIndex,gVertex,MC2_DRAWALPHA | addFlags);
						}
						else
						{
							mcTextureManager->addVertices(theShape->listOfTextures[triType.localTextureHandle].mcTextureNodeIndex,gVertex,MC2_DRAWSOLID | addFlags);
						}
					}
				}
			}
		}
	}

	gos_SetRenderState( gos_State_Fog, 0);
}	

extern bool InEditor;
//------------------------------------------------------------------------------------------------------------
// This function checks each visible triangle against the mouse position and returns if we are over ANY poly!
bool TG_Shape::PerPolySelect (float mouseX, float mouseY)
{
	long j;
	if (!InEditor)
	{
		if (!listOfVertices ||
			!listOfColors ||
			!listOfShadowTVertices ||
			!listOfTriangles ||
			!listOfVisibleFaces ||
			!listOfVisibleShadows ||
			((lastTurnTransformed != (turn-1)) /*&& (lastTurnTransformed != turn)*/))
			return false;
	}
	else
	{
		if (!listOfVertices ||
			!listOfColors ||
			!listOfShadowTVertices ||
			!listOfTriangles ||
			!listOfVisibleFaces ||
			!listOfVisibleShadows ||
			((lastTurnTransformed != (turn-1)) && (lastTurnTransformed != turn)))
			return false;
	}

	TG_TypeShapePtr theShape = (TG_TypeShapePtr)myType;
 	for (j=0;j<numVisibleFaces;j++)
	{
		if (listOfVisibleFaces[j] < numTriangles)
		{
			TG_TypeTriangle triType = theShape->listOfTypeTriangles[listOfVisibleFaces[j]];
			Stuff::Point3D v0,v1,v2;
			v0.x = listOfVertices[triType.Vertices[0]].x;
			v0.y = listOfVertices[triType.Vertices[0]].y;
			
			v1.x = listOfVertices[triType.Vertices[1]].x;
			v1.y = listOfVertices[triType.Vertices[1]].y;
			
			v2.x = listOfVertices[triType.Vertices[2]].x;
			v2.y = listOfVertices[triType.Vertices[2]].y;
			v0.z = v1.z = v2.z = 0.0f;
			
			//Using the above vertex Data, determine if the mouse is over this poly!
			//
			//Theorem:
			// Given the sides of the triangle defined by the lines v0v1, v1v2 and v2v0
			// in the form Ax + By + C = 0
			//
			// the point mousex, mousey lies inside the triangle if and only if
			//
			//  	A0 * mouseX + B0 * mouseY + C0 = D0
			//		A1 * mouseX * B1 * mouseY + c1 = D1
			//		A2 * mouseX + B2 * mouseY + c2 = D2
			//
			// Dx is the same sign for each line as the correct sign for clockwise or counterclockwise vertices!
			//
			Stuff::Vector3D line1;
			Stuff::Vector3D line2;
			line1.Subtract(v0,v1);
			line2.Subtract(v1,v2);
			
			float order = line2.x * line1.y - line1.x * line2.y;
			order = sign(order);
			
			float A0 = -(v0.y - v1.y);
			float B0 = (v0.x - v1.x);
			float C0 = -B0*(v0.y) - A0*(v0.x);
			float D0 = A0 * mouseX + B0 * mouseY + C0;
			
			float A1 = -(v1.y - v2.y);
			float B1 = (v1.x - v2.x);
			float C1 = -B1*(v1.y) - A1*(v1.x);
			float D1 = A1 * mouseX + B1 * mouseY + C1;
 			
 			float A2 = -(v2.y - v0.y);
			float B2 = (v2.x - v0.x);
			float C2 = -B2*(v2.y) - A2*(v2.x);
			float D2 = A2 * mouseX + B2 * mouseY + C2;
			
			if ((sign(D0) == order) && (sign(D0) == sign(D1)) && (sign(D0) == sign(D2)))
				return true;
    	}
	}
	
	return false;
}

#define SHADOW_HARDWARE_BLACK 			0xff000000
#define SHADOW_SOFTWARE_BLACK			0xff000000
#define FLOAT_SHADOW_HARDWARE_BLACK		128.0f
//-------------------------------------------------------------------------------
//This function creates the list of shadows and transforms them in preparation to drawing.
//
void TG_Shape::MultiTransformShadows (Stuff::Point3D *pos, Stuff::LinearMatrix4D *s2w, float rotation)
{
	long j;
	long i;
	if (!numVertices)		//WE are the root Shape which may have no shape or a helper shape which defintely has no shape!
		return;
		
	//Can make this a flag to optimize!
	if (noShadow)
		return;
		
	if (!listOfVertices ||
		!listOfColors ||
		!listOfShadowTVertices ||
		!listOfTriangles ||
		!listOfVisibleFaces ||
		!listOfVisibleShadows)
		return;

	Stuff::Matrix4D identityMatrix;
	identityMatrix.BuildIdentity();
	
	TG_TypeShapePtr theShape = (TG_TypeShapePtr)myType;
	//-----------------------------------------------------------------
	// Run Through listOfVertices and create the listOfShadowVertices
	// for each light source.  Only need to calc infinite rarely if at all.
	// May need some way to delineate this.  Above here, probably!
	//
	// As we calc shadow vertices, transform and store results.
	// 
	// listOfTriangles will simply use these vertices to draw shadows.
	//
	// Again, note that after first iteration of infinite light we only need to transform infinite shadows!
	Stuff::Matrix4D shapeToLocal;
	Stuff::Matrix4D shapeToClip;
	
	long shadowNum = 0;

	memset(shadowsVisible,0,sizeof(bool) * MAX_SHADOWS);

		//-------------------------------------------------------
		// Now, for each light IN RANGE
		// Use formula form Blinn-Trip Down the Graphics Pipeline-Chapter 6
		for (i=0;i<numLights;i++)
		{
			if ((listOfLights[i] != NULL) && (listOfLights[i]->active) && (shadowNum < MAX_SHADOWS))
			{
				switch (listOfLights[i]->lightType)
				{
					case TG_LIGHT_AMBIENT:
					{
						//Ambient light casts no shadows!
					}
					break;

					case TG_LIGHT_INFINITE:
					{
						//----------------------------------------------
						// The Sun casts no shadows at night!!!
						if (!eye->getIsNight())
						{
							Stuff::Vector3D lightDir = rootLightDir[i];
							RotateLight(lightDir,rotation);
							
							for (j=0;j<numVertices;j++)
							{
								long index = j + (shadowNum * numVertices);

								//if (!recalcShadows)
								{
									if (listOfShadowVertices[index].bDataIsNotValid)
									{
										recalcShadows = true;
										listOfShadowVertices[index].bDataIsNotValid = false;
									}
								}

								if (recalcShadows)	//Otherwise, just project last known DATA!!
													//This should almost never be true for any non-moving object.
													//Movers will recalc every frame.  Non-Movers will recalc 
													//when the sun moves enough!
								{
									Stuff::Point3D post = theShape->listOfTypeVertices[j].position;
									//--------------------------------------------------
									Stuff::Vector4D up;
									Stuff::Point3D s_position;
									shapeToLocal.Multiply(*s2w,identityMatrix);
									up.Multiply(post,shapeToLocal);

									//Everything is in WORLD coords now.
									//But this assumes terrain is FLAT and at zero elevation.
									// Thus, we must translate up to tangent plane at terrain position
									up.y -= pos->y;
									//--------------------------------------------------------
									// Check simple case first.  Vertex is on or below ground.
									if (up.y <= 0.0f)
									{
										s_position.x = up.x;
										s_position.z = up.z;
									}
									else
									{
										float zFactor = up.y / lightDir.y;
										s_position.x = up.x - (zFactor * lightDir.x);
										s_position.z = up.z - (zFactor * lightDir.z);
									}
									
									s_position.y = 0.0f;
			
									//-------------------------------------------------------------------
									// Do transform math for the vertex.
									//RotateLight(s_position,rotation);
									//post.Add(s_position,*pos);
									
									Stuff::Vector3D rPos;
									rPos.x = -s_position.x;
									rPos.y = s_position.z;
									rPos.z = land->getTerrainElevation(rPos);
									// PORT M7-B12: water is the visible receiver above submerged
									// terrain. Projecting onto the bottom made the depth-tested
									// shadow fragment and then disappear as the bottom deepened.
									if (rPos.z < Terrain::waterElevation)
										rPos.z = Terrain::waterElevation;
									
									listOfShadowVertices[index].position = rPos;
									//--------------------------------------------------------------------------------
								}
								
								//The transformed position and fog data DO NOT NEED TO EXIST all of time.
								// ONLY the calced position data is invariant.  The other two could come
								// from a common pool and save us SCADS of RAM!!
								// Do this!!!!  I DID!
								// -fs
								eye->projectZ(listOfShadowVertices[index].position,listOfShadowTVertices[index].transformedPosition);
								shadowsVisible[shadowNum] = true;
								
								//eye->projectZ(listOfShadowVertices[index].position,listOfShadowTVertices[index].transformedPosition);
		
								DWORD fogRGB = 0xff000000;
								if (useFog)
								{
									if (pos->y < fogStart)
									{
										float fogFactor = fogStart - pos->y;
										if (fogFactor < 0.0)
											fogRGB = (0xff<<24);
										else
										{
											fogFactor /= (fogStart - fogFull);
											if (fogFactor <= 1.0)
											{
												fogFactor *= fogFactor;
												fogFactor = 1.0 - fogFactor;
												fogFactor *= 256.0;
											}
											else
											{
												fogFactor = 256.0;
											}
		
											unsigned char fogResult = float2long(fogFactor);
											fogRGB = (fogResult << 24);
										}
									}
									else
									{
										fogRGB = (0xff<<24);
									}
								}
								
								//-------------------
								// Distance FOG now.
								if (useFog && Camera::HazeFactor != 0.0f)
								{
									float fogFactor = 1.0 - Camera::HazeFactor;
									DWORD distFog = float2long(fogFactor * 255.0f);
									distFog <<= 24;
									
									if (distFog < fogRGB)
										fogRGB = distFog;
								}

								listOfShadowTVertices[index].fRGBFog = fogRGB;
				   			}

							shadowNum++;
						}
					}
					break;

					case TG_LIGHT_INFINITEWITHFALLOFF:
					{
						//Not planning to use this type of light anymore.  No shadows for now.
					}
					break;

					case TG_LIGHT_POINT:
					case TG_LIGHT_SPOT:
					{
						if (useLocalShadows)
						{
							//------------------------------------------------------------------------------------------
							// Point light does cast a shadow.  LightDir Will change every frame relative to the point.
							// I.e. we DO need to recalc every frame.
							
							//-----------------------------------------------------
							// Check if light source is IN_RANGE!
							Stuff::Point3D vertexToLight;
							vertexToLight = lightDir[i];
							float length = vertexToLight.GetApproximateLength();
							
							vertexToLight = rootLightDir[i];
							float spotLength = listOfLights[i]->maxSpotLength - vertexToLight.GetApproximateLength();
							if (spotLength < 50.0f)
								spotLength = 50.0f;
									
							float falloff = 1.0f;
	
							//Lights do not cast a shadow unless they are intense enough!!
							if (listOfLights[i]->GetFalloff(length, falloff) && (falloff > 0.5f))
							{
								for (j=0;j<numVertices;j++)
								{
									long index = j + (shadowNum * numVertices);
									
									Stuff::Point3D post = theShape->listOfTypeVertices[j].position;
									//--------------------------------------------------
									Stuff::Vector4D up;
									Stuff::Point3D s_position;
									shapeToLocal.Multiply(*s2w,identityMatrix);
									up.Multiply(post,shapeToLocal);
	
									//Everything is in WORLD coords now.
									//But this assumes terrain is FLAT and at zero elevation.
									// Thus, we must translate up to tangent plane at terrain position
									up.y -= pos->y;
	
									shadowsVisible[shadowNum] = true;
									//--------------------------------------------------------
									// Check simple case first.  Vertex is on or below ground.
									if (up.y <= 0.0f)
									{
										s_position.x = up.x;
										s_position.z = up.z;
									}
									else
									{
										float zFactor = up.y / spotLength;
										s_position.x = up.x - (zFactor * -rootLightDir[i].x);
										s_position.z = up.z - (zFactor * -rootLightDir[i].z);
									}
									
									s_position.y = 0.0f;
		
									//-------------------------------------------------------------------
									// Do transform math for the vertex.
									//RotateLight(s_position,rotation);
									//post.Add(s_position,*pos);
	
									Stuff::Vector3D rPos;
									rPos.x = -s_position.x;
									rPos.y = s_position.z;
									rPos.z = land->getTerrainElevation(rPos);
									if (rPos.z < Terrain::waterElevation)
										rPos.z = Terrain::waterElevation;
	
									listOfShadowVertices[index].position = rPos;
									//--------------------------------------------------------------------------------
	
									//The transformed position and fog data DO NOT NEED TO EXIST all of time.
									// ONLY the calced position data is invariant.  The other two could come
									// from a common pool and save us SCADS of RAM!!
									// Do this!!!!  I DID!
									// -fs
									eye->projectZ(listOfShadowVertices[index].position,listOfShadowTVertices[index].transformedPosition);
									shadowsVisible[shadowNum] = true;
	
									DWORD fogRGB = 0xff000000;
									if (useFog)
									{
										if (pos->y < fogStart)
										{
											float fogFactor = fogStart - pos->y;
											if (fogFactor < 0.0)
												fogRGB = (0xff<<24);
											else
											{
												fogFactor /= (fogStart - fogFull);
												if (fogFactor <= 1.0)
												{
													fogFactor *= fogFactor;
													fogFactor = 1.0 - fogFactor;
													fogFactor *= 256.0;
												}
												else
												{
													fogFactor = 256.0;
												}
			
												unsigned char fogResult = float2long(fogFactor);
												fogRGB = (fogResult << 24);
											}
										}
										else
										{
											fogRGB = (0xff<<24);
										}
									}
									
									//-------------------
									// Distance FOG now.
									if (useFog && Camera::HazeFactor != 0.0f)
									{
										float fogFactor = 1.0 - Camera::HazeFactor;
										DWORD distFog = float2long(fogFactor * 255.0f);
										distFog <<= 24;
										
										if (distFog < fogRGB)
											fogRGB = distFog;
									}
	
									listOfShadowTVertices[index].fRGBFog = fogRGB;
								}
	
								shadowNum++;
							}
							else
							{
								shadowsVisible[shadowNum] = false;
							}
						}
					}
					break;
				}
			}
		}

	numVisibleShadows = 0;			//Reset Visible Shadows

	long totalShadows = 0;
	// PORT: was `long j=0;`. `j` is already declared at the top of this function
	// (hoisted out of an earlier for-init when the pre-standard MSVC for-scope
	// leak was removed), so re-declaring it here is a redefinition in the same
	// scope. Assignment only.
	j=0;
	for (j=0;j<MAX_SHADOWS;j++)
		if (shadowsVisible[j])
			totalShadows++;
		
	if (totalShadows)
	{
		for (j=0;j<numTriangles;j++)
		{
			listOfVisibleShadows[numVisibleShadows] = j;
			numVisibleShadows++;
			
			TG_TypeTriangle triType = theShape->listOfTypeTriangles[j];
			
			//--------------------------------------------------
			// Any sorting we need to do occurs in here as well!
			// So, find out what texture this face is using and mark that texture as needing 3 more vertices.
			if (theShape->listOfTextures[triType.localTextureHandle + (theShape->numTextures>>1)].gosTextureHandle != 0xffffffff)
			{
				for (long k=0;k<totalShadows;k++)
					mcTextureManager->addTriangle(theShape->listOfTextures[triType.localTextureHandle + (theShape->numTextures>>1)].mcTextureNodeIndex,MC2_DRAWALPHA | MC2_ISSHADOWS);
			}
			else
			{
				for (long k=0;k<totalShadows;k++)
					mcTextureManager->addTriangle(theShape->listOfTextures[triType.localTextureHandle].mcTextureNodeIndex,MC2_DRAWALPHA | MC2_ISSHADOWS);
			}
		}
	}
}	

//------------------------------------------------------------------------------------------------------------------------------------
// This functions draws the shadows.  May want zCompare and zWrite off at this point, hence separate render to facilitate draw order.
// So shadows fall across all 3d objects on terrain.
long TG_Shape::RenderShadows (long startFace)
{
	long j;
	long i;
	if (!renderTGLShapes)
		return(startFace);
		
 	if (!numVertices)		//WE are the root Shape which may have no shape or a helper shape which defintely has no shape!
		return(startFace);
		
	if (!numVisibleShadows)
		return startFace;
		
	//Can make this a flag to optimize!
	if (strnicmp(getNodeName(),"SpotLight_",10) == 0)
		return startFace;
		
	if (!listOfVertices ||
		!listOfColors ||
		!listOfShadowTVertices ||
		!listOfTriangles ||
		!listOfVisibleFaces ||
		!listOfVisibleShadows ||
		(/*(lastTurnTransformed != (turn-1)) &&*/ (lastTurnTransformed != turn)))
		return startFace;

 	if (fogColor != 0xffffffff)
	{
		gos_SetRenderState( gos_State_Fog, (intptr_t)&(fogColor));
	}
	else
	{
		gos_SetRenderState( gos_State_Fog, 0);
	}

	TG_TypeShapePtr theShape = (TG_TypeShapePtr)myType;
	for (i=0;i<MAX_SHADOWS;i++)
	{
		if (shadowsVisible[i])
		{
			for (j=0;j<numVisibleShadows && listOfShadowTVertices;j++)
			{
				DWORD realStart = (j + startFace) % 64;
				if (listOfVisibleShadows[j] < numTriangles)
				{

					TG_TypeTriangle triType = theShape->listOfTypeTriangles[listOfVisibleShadows[j]];
	
					TG_ShadowVertexTemp vertex0 = listOfShadowTVertices[triType.Vertices[0] + (i * numVertices)];
					TG_ShadowVertexTemp vertex1 = listOfShadowTVertices[triType.Vertices[1] + (i * numVertices)];
					TG_ShadowVertexTemp vertex2 = listOfShadowTVertices[triType.Vertices[2] + (i * numVertices)];
	
					gos_VERTEX gVertex[3];
	
					gVertex[0].x		= vertex0.transformedPosition.x;
					gVertex[0].y		= vertex0.transformedPosition.y;
					gVertex[0].z		= vertex0.transformedPosition.z + (1.0f/60000.0f)*(realStart);
					gVertex[0].rhw		= vertex0.transformedPosition.w;
					gVertex[0].u		= triType.uvdata.u0;
					gVertex[0].v		= triType.uvdata.v0;
					gVertex[0].argb		= 0x3f000000;
					gVertex[0].frgb		= vertex0.fRGBFog;
	
					gVertex[1].x		= vertex1.transformedPosition.x;
					gVertex[1].y		= vertex1.transformedPosition.y;
					gVertex[1].z		= vertex1.transformedPosition.z + (1.0f/60000.0f)*(realStart);
					gVertex[1].rhw		= vertex1.transformedPosition.w;
					gVertex[1].u		= triType.uvdata.u1;
					gVertex[1].v		= triType.uvdata.v1;
					gVertex[1].argb		= 0x3f000000;
					gVertex[1].frgb		= vertex1.fRGBFog;
	
					gVertex[2].x		= vertex2.transformedPosition.x;
					gVertex[2].y		= vertex2.transformedPosition.y;
					gVertex[2].z		= vertex2.transformedPosition.z + (1.0f/60000.0f)*(realStart);
					gVertex[2].rhw		= vertex2.transformedPosition.w;
					gVertex[2].u		= triType.uvdata.u2;
					gVertex[2].v		= triType.uvdata.v2;
					gVertex[2].argb		= 0x3f000000;
					gVertex[2].frgb		= vertex2.fRGBFog;
	
					if (drawOldWay)
					{
						if (theShape->listOfTextures[triType.localTextureHandle + (theShape->numTextures>>1)].gosTextureHandle != 0xffffffff)
							gos_SetRenderState( gos_State_Texture, theShape->listOfTextures[triType.localTextureHandle + (theShape->numTextures>>1)].gosTextureHandle );
						else if (theShape->listOfTextures[triType.localTextureHandle].gosTextureHandle != 0xffffffff)
							gos_SetRenderState( gos_State_Texture, theShape->listOfTextures[triType.localTextureHandle].gosTextureHandle );
						else
							gos_SetRenderState( gos_State_Texture, 0 );
							
						if ((gVertex[0].z >= 0.0f) &&
							(gVertex[0].z < 1.0f) &&
							(gVertex[1].z >= 0.0f) &&  
							(gVertex[1].z < 1.0f) && 
							(gVertex[2].z >= 0.0f) &&  
							(gVertex[2].z < 1.0f))
						{
							gos_DrawTriangles(gVertex, 3);
						}
					}
					else
					{
						if ((gVertex[0].z >= 0.0f) &&
							(gVertex[0].z < 1.0f) &&
							(gVertex[1].z >= 0.0f) &&  
							(gVertex[1].z < 1.0f) && 
							(gVertex[2].z >= 0.0f) &&  
							(gVertex[2].z < 1.0f))
						{
							//--------------------------------------------------
							// Any sorting we need to do occurs in here as well!
							// So, find out what texture this face is using and mark that texture as needing 3 more vertices.
							if (theShape->listOfTextures[triType.localTextureHandle + (theShape->numTextures>>1)].gosTextureHandle != 0xffffffff)
							{
								mcTextureManager->addVertices(theShape->listOfTextures[triType.localTextureHandle + (theShape->numTextures>>1)].mcTextureNodeIndex,gVertex,MC2_ISSHADOWS | MC2_DRAWALPHA);
							}
							else
							{
								mcTextureManager->addVertices(theShape->listOfTextures[triType.localTextureHandle].mcTextureNodeIndex,gVertex,MC2_ISSHADOWS | MC2_DRAWALPHA);
							}
						}
					}
				}
			}
		}
	}

	gos_SetRenderState( gos_State_Fog, 0);

	return(startFace + numVisibleShadows);
}

//-------------------------------------------------------------------------------
