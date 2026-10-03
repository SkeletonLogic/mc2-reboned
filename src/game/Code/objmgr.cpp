//---------------------------------------------------------------------------
//
//	objmgr.cpp - This file contains the GameObject Manager class code
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
// Include Files

//=============
#ifndef MCLIB_H
#include "mclib.h"
#include <chrono>   // PORT (perf): MC2_OBJ_SPLIT
#endif

#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>

#ifndef DOBJMGR_H
#include "dobjmgr.h"
#endif

#ifndef OBJMGR_H
#include "objmgr.h"
#endif

#ifndef OBJTYPE_H
#include "Objtype.h" // PORT: match case-sensitive host path.
#endif

#ifndef GAMEOBJ_H
#include "gameobj.h"
#endif

#ifndef BLDNG_H
#include "bldng.h"
#endif

#ifndef UNITDESG_H
#include "unitdesg.h"
#endif

#ifndef ARTLRY_H
#include "artlry.h"
#endif

#ifndef TURRET_H
#include "turret.h"
#endif

#ifndef GROUP_H
#include "group.h"
#endif

#ifndef TEAM_H
#include "Team.h" // PORT: match case-sensitive host path.
#endif

#ifndef COMNDR_H
#include "comndr.h"
#endif

#ifndef MECH_H
#include "mech.h"
#endif

#ifndef GVEHICL_H
#include "gvehicl.h"
#endif

#ifndef TERROBJ_H
#include "terrobj.h"
#endif

#ifndef WEAPONBOLT_H
#include "weaponbolt.h"
#endif

#ifndef CARNAGE_H
#include "carnage.h"
#endif

#ifndef LIGHT_H
#include "light.h"
#endif

#ifndef GATE_H
#include "gate.h"
#endif

#ifdef USE_ELEMENTALS
#ifndef ELEMNTL_H
#include "elemntl.h"
#endif
#endif

#ifndef COLLSN_H
#include "Collsn.h" // PORT: match case-sensitive host path.
#endif

#include <mc2_port.h> // PORT: M7 A1b ObjectManager teardown census.
#ifndef WEAPONFX_H
#include "weaponfx.h"
#endif

#ifndef MISSION_H
#include "mission.h"
#endif

#define BRIDGE_OBJTYPE				448
#define MINE1						60
#define MINE2						251
#define MINE3						521
#define MINE4						522
#define MINE5						610
#define MINE6						611
#define MINE7						612

//---------------------------------------------------------------------------
// Static Globals
#if 1	// ids can be 8 long, INCLUDING TERMINATOR!!!!!
char DEFAULT_LIST_ID[] = "DEFAULT";
char CLANMECH_LIST_ID[] = "CLANMEC";
char ISMECH_LIST_ID[] = "ISMECH";
char ICON_LIST_ID[] = "ICONS";
char WEAPON_LIST_ID[] = "WEAPON";
#else
char DEFAULT_LIST_ID[] = "DEFAULT!";
char CLANMECH_LIST_ID[] = "CLANMECH";
char ISMECH_LIST_ID[] = "ISMECH";
char ICON_LIST_ID[] = "ICONS";
char WEAPON_LIST_ID[] = "WEAPON";
#endif
//long ObjectQueue::objectsInList = 0;

// PORT: declared as arrays here but DEFINED as pointers in MCLib/Terrain.cpp:137-138
// (long *usedBlockList;). In C++ those mangle differently, so the link never
// resolved. Declarations corrected to match the definitions; x[i] indexing is
// identical either way.
extern long *usedBlockList;			//Trust ME~~!!!!!!!!!!!!!!!!!!!!!!!!
extern long *moverBlockList;			//Trust ME~~!!!!!!!!!!!!!!!!!!!!!!!!  AGAIN 
extern bool updateTerrainObjects;
extern bool	updateObjects;
extern bool	renderTerrainObjects;
extern bool	renderObjects;
extern GameObjectPtr MoverRoster[MAX_MOVER_PART_ID - MIN_MOVER_PART_ID + 1];
extern bool MaxObjectsDrawn;
extern bool drawOldWay;

/*
ObjectQueuePtr objectList = NULL;
ObjectQueueNodePtr clanMechList = NULL;
ObjectQueueNodePtr innerSphereMechList = NULL;
ObjectQueueNodePtr iconList = NULL;
ObjectQueueNodePtr weaponList = NULL;
*/

#define	VISIBLE_THRESHOLD	1

GameObjectManagerPtr ObjectManager = NULL;
GameObjectPtr* collisionList = NULL;
long numCollidables = 0;
//***************************************************************************
//* MISC routines
//***************************************************************************

bool blockInList (long blockNum) 
{
	long i;
	long totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;

	for (i = 0; i < totalBlocks; i++) {
		if (usedBlockList[i] == blockNum)
			return(TRUE);
		else if (usedBlockList[i] == -1)
			return(FALSE);
	}
	
	return(FALSE);
}

//---------------------------------------------------------------------------

bool moverInList (long blockNum) 
{
	long i;
	long totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;

	for (i = 0; i < totalBlocks; i++) {
		if (moverBlockList[i] == blockNum)
			return(TRUE);
		else if (moverBlockList[i] == -1)
			return(FALSE);
	}
	
	return(FALSE);
}

//***************************************************************************
//* GAMEOBJECT MANAGER class
//***************************************************************************

void* GameObjectManager::operator new (size_t ourSize) {

	void* result = systemHeap->Malloc(ourSize);
	if (!result) 
	{
		Fatal(0, " GameObjectManager.new: unable to create GameObject Manager ");
	}

	return(result);
}

//---------------------------------------------------------------------------

void GameObjectManager::operator delete (void* us) {

	systemHeap->Free(us);
}

//---------------------------------------------------------------------------

void GameObjectManager::init (void) {
	long i;

	objTypeManager = NULL;

	numMechs = 0;
	numVehicles = 0;
	numElementals = 0;
	numTerrainObjects = 0;
	numBuildings = 0;
	numTurrets = 0;
	numWeapons = 0;
	numCarnage = 0;
	numLights = 0;
	numGates = 0;
	numArtillery = 0;

	numGateControls = 0;
	numTurretControls = 0;
	numPowerGenerators = 0;
	numSpecialBuildings = 0;

	//carnageCount = 0;
	Terrain::numObjBlocks = 0;

	mechs = NULL;
	vehicles = NULL;
	elementals = NULL;
	terrainObjects = NULL;
	buildings = NULL;
	turrets = 0;
	weapons = NULL;
	carnage = NULL;
	lights = NULL;
	artillery = NULL;
	gates = NULL;

	objList = NULL;
	collidableList = NULL;
	numCollidables = 0;
	numGoodMovers = 0;
	numBadMovers = 0;
	numMovers = 0;
	nextReinforcementPartId = MIN_REINFORCEMENT_PART_ID;
	numRemoved = 0;
	nextWatchID = 1;

	currentWeaponsIndex = 0;
	currentCarnageIndex = 0;
	currentLightIndex = 0;
	currentArtilleryIndex = 0;
	
	long totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;

	for (i = 0; i < totalBlocks; i++) {
		Terrain::objBlockInfo[i].active = false;
		Terrain::objBlockInfo[i].firstHandle = 0;
		Terrain::objBlockInfo[i].numObjects = 0;
	}

}

//---------------------------------------------------------------------------

void GameObjectManager::init (char* objTypeDataFile, long objTypeHeapSize, long objHeapSize) {

	if (objTypeManager)
		delete objTypeManager;

	objTypeManager = new ObjectTypeManager;
	if (!objTypeManager)
		Fatal(0, " GameObjectManager.init: unable to create objTypeManager ");
	long result = objTypeManager->init(objTypeDataFile, objTypeHeapSize, objHeapSize);
	if (result != NO_ERR)
		Fatal(0, " GameObjectManager.init: unable to init objTypeManager ");
}

//---------------------------------------------------------------------------

void GameObjectManager::setNumObjects (long nMechs,
									   long nVehicles,
									   long nElementals,
									   long nTerrainObjects,
									   long nBuildings,
									   long nTurrets,
									   long nWeapons,
									   long nCarnage,
									   long nLights,
									   long nArtillery,
									   long nGates) 
{

	long i=0;

	numMechs = nMechs;
	numVehicles = nVehicles;
	numElementals = nElementals;

	//-------------------------------------------------------------------------
	// We need to make room for a potential max # of reinforcements per team...
	//
	// Please DONT add 2 to each of these.   You wind up pointing into the next list!!!!
	// -fs   6/14/2000
	maxMechs = numMechs + MAX_TEAMS * MAX_REINFORCEMENTS_PER_TEAM;
	maxVehicles = numVehicles + MAX_TEAMS * MAX_REINFORCEMENTS_PER_TEAM;
	maxMovers = maxMechs + maxVehicles + numElementals;

	if (nTerrainObjects > -1)
		numTerrainObjects = nTerrainObjects;
	if (nBuildings > -1)
		numBuildings = nBuildings;
	if (nTurrets > -1)
		numTurrets = nTurrets;
	if (nWeapons > -1)
		numWeapons = nWeapons;
	if (nCarnage > -1)
		numCarnage = nCarnage;
	if (nLights > -1)
		numLights = nLights;
	if (nArtillery > -1)
		numArtillery = nArtillery;
	if (nGates > -1)
		numGates = nGates;

	GameObject::setInitialize(true);

	//-----------------------------------------------------------
	// First element in list is NULL (handle of 0 is always NULL)
	objList = (GameObjectPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(GameObjectPtr) * (getMaxObjects() + 1));
	memset(objList,0,sizeof(GameObjectPtr) * (getMaxObjects() + 1));

	watchList = (GameObjectPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(GameObjectPtr) * (getMaxObjects() + 1));
	memset(watchList,0,sizeof(GameObjectPtr) * (getMaxObjects() + 1));

	long curHandle = 1;
	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (maxMechs > 0) {
		mechs = (BattleMechPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(BattleMechPtr) * maxMechs);
		if (!mechs)
			Fatal(maxMechs, " GameObjectManager.setNumObjects: cannot malloc mechs ");
		for (i = 0; i < maxMechs; i++) {
			mechs[i] = new BattleMech;
			mechs[i]->setHandle(curHandle);
			objList[curHandle++] = mechs[i];
		}
		for (i = numMechs; i < maxMechs; i++)
			mechs[i]->setExists(false);
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (maxVehicles > 0) {
		vehicles = (GroundVehiclePtr*)ObjectTypeManager::objectCache->Malloc(sizeof(GroundVehiclePtr) * maxVehicles);
		if (!vehicles)
			Fatal(maxVehicles, " GameObjectManager.setNumObjects: cannot malloc vehicles ");
		for (i = 0; i < maxVehicles; i++) {
			vehicles[i] = new GroundVehicle;
			vehicles[i]->setHandle(curHandle);
			objList[curHandle++] = vehicles[i];
		}
		for (i = numVehicles; i < maxVehicles; i++)
			vehicles[i]->setExists(false);
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
#ifdef USE_ELEMENTALS
	if (numElementals > 0) {
		elementals = (ElementalPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(ElementalPtr) * numElementals);
		if (!elementals)
			Fatal(numElementals, " GameObjectManager.setNumObjects: cannot malloc elementals ");
		for (i = 0; i < numElementals; i++) {
			elementals[i] = new Elemental;
			elementals[i]->setHandle(curHandle);
			objList[curHandle++] = elementals[i];
		}
	}
#endif

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (numTerrainObjects > 0) {
		terrainObjects = (TerrainObjectPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(TerrainObjectPtr) * numTerrainObjects);
		if (!terrainObjects)
			Fatal(numTerrainObjects, " GameObjectManager.setNumObjects: cannot malloc terrain objects ");

		for (i = 0; i < numTerrainObjects; i++) {
			terrainObjects[i] = new TerrainObject;
			terrainObjects[i]->setHandle(curHandle);
			objList[curHandle++] = terrainObjects[i];
		}
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (numBuildings > 0) {
		buildings = (BuildingPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(BuildingPtr) * numBuildings);
		if (!buildings)
			Fatal(numBuildings, " GameObjectManager.setNumObjects: cannot malloc buildings ");
		for (i = 0; i < numBuildings; i++) {
			buildings[i] = new Building;
			buildings[i]->setHandle(curHandle);
			objList[curHandle++] = buildings[i];
		}
	}

	if (numTurrets > 0)
	{
		turrets = (TurretPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(TurretPtr) * numTurrets);
		if ( !turrets )
			Fatal(numTurrets, " GameObjectManager.setNumObjects: cannot malloc Turrets ");
			
		for (i = 0; i < numTurrets; i++) 
		{
			turrets[i] = new Turret;
			turrets[i]->setHandle(curHandle);
			objList[curHandle++] = turrets[i];
		}
	}

	if (numGates > 0)
	{
		gates = (GatePtr*)ObjectTypeManager::objectCache->Malloc(sizeof(GatePtr) * numGates);
		if ( !gates )
			Fatal(numGates, " GameObjectManager.setNumObjects: cannot malloc Gates ");
			
		for (i = 0; i < numGates; i++) 
		{
			gates[i] = new Gate;
			gates[i]->setHandle(curHandle);
			objList[curHandle++] = gates[i];
		}
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (numWeapons > 0) {
		weapons = (WeaponBoltPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(WeaponBoltPtr) * numWeapons);
		if (!weapons)
			Fatal(numWeapons, " GameObjectManager.setNumObjects: cannot malloc weapons ");
		for (i = 0; i < numWeapons; i++) {
			weapons[i] = new WeaponBolt;
			weapons[i]->setHandle(curHandle);
			objList[curHandle++] = weapons[i];
		}
	}

	if (numCarnage > 0) {
		carnage = (CarnagePtr*)ObjectTypeManager::objectCache->Malloc(sizeof(CarnagePtr) * numCarnage);
		if (!carnage)
			Fatal(numCarnage, " GameObjectManager.setNumObjects: cannot malloc carnage ");
		for (i = 0; i < numCarnage; i++) {
			carnage[i] = new Carnage;
			carnage[i]->setHandle(curHandle);
			objList[curHandle++] = carnage[i];
		}
	}

	if (numArtillery > 0) {
		artillery = (ArtilleryPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(ArtilleryPtr) * numArtillery);
		if (!artillery)
			Fatal(numArtillery, " GameObjectManager.setNumObjects: cannot malloc artillery ");
		for (i = 0; i < numArtillery; i++) {
			artillery[i] = new Artillery;
			artillery[i]->setHandle(curHandle);
			objList[curHandle++] = artillery[i];
		}
	}

	useMoverLineOfSightTable = true;
	moverLineOfSightTable = (char*)systemHeap->Malloc(maxMovers * maxMovers);
	if (!moverLineOfSightTable)
		Fatal(numGates, " GameObjectManager.setNumObjects: cannot malloc moverLineOfSightTable ");

	GameObject::setInitialize(false);
}

//---------------------------------------------------------------------------

BattleMechPtr GameObjectManager::getMech (long mechIndex) {

	if (!mechs || (mechIndex < 0) || (mechIndex >= numMechs))
		return(NULL);

	return(mechs[mechIndex]);
}

//---------------------------------------------------------------------------

GroundVehiclePtr GameObjectManager::getVehicle (long vehicleIndex) {

	if (!vehicles || (vehicleIndex < 0) || (vehicleIndex >= numVehicles))
		return(NULL);

	return(vehicles[vehicleIndex]);
}

//---------------------------------------------------------------------------

GroundVehiclePtr GameObjectManager::getOpenVehicle (void)
{
	long i;
	for (i=0;i<maxVehicles;i++)
	{
		if (!vehicles[i]->getExists())
		{
			if (i > numVehicles)
				numVehicles = i;

			return vehicles[i];
		}
	}

	return NULL;
}

//---------------------------------------------------------------------------

ElementalPtr GameObjectManager::getElemental (long elementalIndex) {

	if (!elementals || (elementalIndex < 0) || (elementalIndex >= numElementals))
		return(NULL);

	return(elementals[elementalIndex]);
}

//---------------------------------------------------------------------------

BattleMechPtr GameObjectManager::newMech (void) {

	if (numMechs < maxMechs) {
		mechs[numMechs]->init(false);
		mechs[numMechs]->setExists(true);
		setPartId(mechs[numMechs], -1, -1, -1);
		mechs[numMechs]->watchID = 0;
		
		//Gotta rebuild the list or the new stuff don't show up!
		// This flag will force rebuild at next collision check
		rebuildCollidableList = true;
		
		return(mechs[numMechs++]);
	}

	// PORT: returning NULL here makes Mission::addMover skip the whole mover
	// silently -- no object, no team roster entry, no unit bar icon, no model,
	// no message. A player mech simply is not there. Say so.
	printf("[objmgr] MECH POOL FULL: numMechs=%ld maxMechs=%ld -- mover NOT created\n",
	       numMechs, maxMechs);
	fflush(stdout);
	return(NULL);
}

//---------------------------------------------------------------------------

GroundVehiclePtr GameObjectManager::newVehicle (void) {

	if (numVehicles < maxVehicles) {
		vehicles[numVehicles]->init(false);
		vehicles[numVehicles]->setExists(true);
		setPartId(vehicles[numVehicles], -1, -1, -1);
		vehicles[numVehicles]->watchID = 0;
		
		//Gotta rebuild the list or the new stuff don't show up!
		// This flag will force rebuild at next collision check
		rebuildCollidableList = true;
		
 		return(vehicles[numVehicles++]);
	}

	// PORT (M8-B45): the vehicle pool had no equivalent of the MECH POOL FULL
	// report above, so a mission that ran out of vehicle slots said nothing at
	// all -- the mover was skipped, with no object, no roster entry, no icon and
	// no message. The owner asked directly whether resuming a quicksave in a
	// unit-heavy mission caps how many units come back; this is the place that
	// would silently do it, and now it answers.
	printf("[objmgr] VEHICLE POOL FULL: numVehicles=%ld maxVehicles=%ld "
	       "-- mover NOT created\n", numVehicles, maxVehicles);
	fflush(stdout);
	return(NULL);
}

//---------------------------------------------------------------------------

void GameObjectManager::setWatchID (GameObjectPtr obj) {

	if (obj->watchID == 0) {
		// PORT (M8-B43): THIS WROTE PAST THE END OF watchList AND NOTHING
		// STOPPED IT.
		//
		// watchList is allocated once, getMaxObjects() + 1 entries
		// (objmgr.cpp:359). nextWatchID starts at 1 and ONLY EVER INCREASES:
		// freeMover clears the slot and zeroes the object's id, but it never
		// reuses the number and never decrements the counter. So the counter
		// does not measure how many objects exist, it measures how many have
		// EVER been given an id -- every weapon fired, every piece of carnage,
		// every vehicle pilot that bails out of a wreck. Those pools are
		// recycled; their watch ids are not.
		//
		// Once nextWatchID passes getMaxObjects(), the line below was an
		// out-of-bounds heap WRITE, once per new object, forever after. That is
		// silent corruption of whatever the allocator put next to watchList,
		// and this project has already lost sessions to exactly that shape of
		// defect (M7-B1).
		//
		// IT WAS NOT THE CAUSE OF EITHER CRASH, AND THAT IS NOW MEASURED, NOT
		// ASSUMED. The 2026-09-08 session reported `nextWatchID 187` against a
		// capacity of 3,213 -- under 6% -- while crashing on a dangling watch
		// id. A 20,000-tick headless mc2_10 with combat uses 100-199. So the
		// exhaustion theory raised here on 2026-09-07 is STRUCK: it never got
		// near the bound, and the real cause was freeMover not telling pilots
		// their vehicle had gone.
		//
		// The bound stays anyway. The write genuinely had no limit, which is a
		// defect on its own merits whether or not it has ever fired.
		if (nextWatchID > (unsigned long)getMaxObjects()) {
			static long reported = 0;
			if (reported < 8) {
				++reported;
				printf("[watchid] EXHAUSTED: nextWatchID %lu is past the "
				       "watchList capacity %ld. Refusing to write out of "
				       "bounds; this object gets no watch id.%s\n",
				       nextWatchID, getMaxObjects(),
				       (reported == 8) ? " (further reports suppressed)" : "");
				fflush(stdout);
			}
			return;
		}
		watchList[nextWatchID] = obj;
		obj->watchID = nextWatchID++;

		// PORT (M8-B43): the headroom measurement, and it is designed to come
		// back NEGATIVE. If a real session never crosses 50%, the exhaustion
		// theory above is dead and should be struck from the register rather
		// than carried around. One line per band, four lines a session at most.
		{
			static long band = 0;
			const long cap = getMaxObjects();
			const long pct = (cap > 0) ? (long)((nextWatchID * 100) / (unsigned long)cap) : 0;
			const long bands[3] = { 50, 75, 90 };
			while (band < 3 && pct >= bands[band]) {
				printf("[watchid] high water: %lu of %ld watch ids used (%ld%%)\n",
				       nextWatchID, cap, pct);
				fflush(stdout);
				++band;
			}

			// The bands answer "is it close?". They cannot answer "how close?",
			// and a silent run is only worth something if the number behind the
			// silence is known. This traces the actual figure, off by default
			// so the strict bar's hashed stdout does not move.
			static const bool census = []() {
				const char* e = getenv("MC2_WATCHID_CENSUS");
				return e && *e && strcmp(e, "0") != 0;
			}();
			if (census && (nextWatchID <= 2 || (nextWatchID % 100) == 0)) {
				printf("[watchid] census: %lu of %ld used (%ld%%)\n",
				       nextWatchID, cap, pct);
				fflush(stdout);
			}
		}
	}
}

//---------------------------------------------------------------------------

void GameObjectManager::freeMover (MoverPtr mover) {

	bool foundIt = modifyMoverLists(mover, MOVERLIST_DELETE);
	if (foundIt) {
		// PORT (M8-B43): TELL THE PILOT ITS VEHICLE IS GONE.
		//
		// ORDER MATTERS AND I GOT IT WRONG FIRST TIME: this has to run BEFORE
		// release(), because Mover::release() does `freeWarrior(pilot); pilot =
		// NULL;`. Written after release() the getPilot() test is always false
		// and the whole block is a silent no-op that looks like a fix.
		//
		// This cleared the watch slot and zeroed the object's own id, and told
		// NOBODY. A MechWarrior keeps its vehicle as a watch id (vehicleWID),
		// so every pilot still pointing at this mover was left holding an id
		// that resolves to nothing -- and getVehicle()'s callers do not check.
		//
		// MEASURED, the owner's 2026-09-08 crash, from the [pilot-vehicle]
		// probe added for exactly this question:
		//   'holds vehicleWID 176 but it does not resolve: slot is empty --
		//    the object was freed and the slot cleared (nextWatchID 187)'
		// twenty of them, ids 174-183 in one burst -- a chain explosion taking
		// several units at once. nextWatchID 187 against a capacity of 3,213
		// also KILLS the watch-id exhaustion theory from the previous commit:
		// it was nowhere near, and that hypothesis is struck.
		//
		// The dangling id is what crashed warrior.cpp:5116
		// (`myVehicle->isCaptured()`), reached from GroundVehicle::update ->
		// updateAIControl -> mainDecisionTree, because the wrecked vehicle goes
		// on calling its pilot after the pilot's CURRENT vehicle -- the bailed
		// crewman object from createVehiclePilot -- has been destroyed.
		//
		// LIMIT, STATED BECAUSE IT IS NOT SMALL: a bailed crewman created by
		// createVehiclePilot SHARES its MechWarrior with the wreck it came out
		// of, and release() below frees that warrior while the wreck still
		// holds a pointer to it. This clears the dangling VEHICLE id; it does
		// not fix the shared-warrior lifetime. That is why the guards in
		// mainDecisionTree and updateActions stay.
		if (mover->getPilot() && mover->getPilot()->getVehicle() == mover)
			mover->getPilot()->setVehicle(NULL);

		mover->release();
		mover->setExists(false);
		mover->setFlag(OBJECT_FLAG_REMOVED, true);
		mover->setPartId(0);

		watchList[mover->watchID] = NULL;
		mover->watchID = 0;
	}
}

//---------------------------------------------------------------------------

void GameObjectManager::tradeMover (MoverPtr mover, long newTeamID, long newCommanderID) {

	if (newTeamID > -1) {
		if (mover->teamId != newTeamID)
			mover->salvaged = true;
		if (mover->getObjectClass() == BATTLEMECH) {
			((BattleMech*)mover)->killed = false;
			((BattleMech*)mover)->lost = false;
		}
		mover->tradeRefresh();
	}
	mover->setTeamId(newTeamID, true);
	if (mover->getGroup())
		mover->getGroup()->remove(mover);
	mover->watchID = 0;
	if (newTeamID > -1)
		Team::teams[newTeamID]->addToRoster(mover);
	mover->setCommanderId(newCommanderID);
	modifyMoverLists(mover, MOVERLIST_TRADE);
}

//---------------------------------------------------------------------------

#ifdef USE_ELEMENTALS

ElementalPtr GameObjectManager::addElemental (void) {

	if (numMechs < maxMechs) {
		elementals[numElementals]->setExists(true);
		setPartId(elementals[numElementals], -1, -1, -1);
		return(elementals[numElementals++]);
	}
	return(NULL);
}

#endif

//---------------------------------------------------------------------------

TerrainObjectPtr GameObjectManager::getTerrainObject (long terrainObjectIndex) {

	if (!terrainObjects || (terrainObjectIndex < 0) || (terrainObjectIndex >= numTerrainObjects))
		return(NULL);

	return(terrainObjects[terrainObjectIndex]);
}

//---------------------------------------------------------------------------

BuildingPtr GameObjectManager::getBuilding (long buildingIndex) {

	if (!buildings || (buildingIndex < 0) || (buildingIndex >= numBuildings))
		return(NULL);

	return(buildings[buildingIndex]);
}

//---------------------------------------------------------------------------

TurretPtr GameObjectManager::getTurret (long turretIndex) 
{
	if (!turrets || (turretIndex < 0) || (turretIndex >= numTurrets))
		return(NULL);

	return(turrets[turretIndex]);
}

//---------------------------------------------------------------------------

GatePtr GameObjectManager::getGate (long gateIndex) 
{
	if (!gates || (gateIndex < 0) || (gateIndex >= numGates))
		return(NULL);

	return(gates[gateIndex]);
}

//---------------------------------------------------------------------------

WeaponBoltPtr GameObjectManager::getWeapon (void) 
{
	if (!weapons || (currentWeaponsIndex < 0) || (currentWeaponsIndex >= numWeapons))
		return(NULL);

	currentWeaponsIndex++;
	if (currentWeaponsIndex >= numWeapons)
		currentWeaponsIndex = 0;

	//Make sure this weapon has an opportunity to damage its target
	weapons[currentWeaponsIndex]->finishNow();	

	return(weapons[currentWeaponsIndex]);
}

//---------------------------------------------------------------------------

CarnagePtr GameObjectManager::getCarnage (CarnageEnumType carnageType) {

	if (!carnage || (currentCarnageIndex < 0) || (currentCarnageIndex >= numCarnage))
		return(NULL);

	currentCarnageIndex++;
	if (currentCarnageIndex >= numCarnage)
		currentCarnageIndex = 0;

	carnage[currentCarnageIndex]->finishNow();
	carnage[currentCarnageIndex]->init(carnageType);
	
	return(carnage[currentCarnageIndex]);
}

//---------------------------------------------------------------------------

void GameObjectManager::releaseCarnage (CarnagePtr obj) {

	obj->setExists(false);
	obj->setOwner(NULL);
}

//---------------------------------------------------------------------------

LightPtr GameObjectManager::getLight (void) {

	if (!lights || (currentLightIndex < 0) || (currentLightIndex >= numLights))
		return(NULL);

	if (currentLightIndex >= numLights)
		currentLightIndex = 0;

//	lights[currentLightIndex]->finishNow();
	lights[currentLightIndex]->init(false);
	lights[currentLightIndex]->setExists(true);
	return(lights[currentLightIndex++]);
}

//---------------------------------------------------------------------------

void GameObjectManager::releaseLight (LightPtr obj) {

	obj->setExists(false);
//	obj->setOwner(NULL);
}

//---------------------------------------------------------------------------

ArtilleryPtr GameObjectManager::getArtillery (void) 
{
	if (!artillery || (currentArtilleryIndex < 0) || (currentArtilleryIndex >= numArtillery))
		STOP(("Artillery Strikes are out of range in ObjectManager %d",currentArtilleryIndex));

	currentArtilleryIndex++;
	if (currentArtilleryIndex >= numArtillery)
		currentArtilleryIndex = 0;

	//OK to return NULL now.  Lets Multiplayer know that there are no more strikes available.
	if (artillery[currentArtilleryIndex]->getExists())
		return NULL;
		
	artillery[currentArtilleryIndex]->init(false);
	artillery[currentArtilleryIndex]->setExists(true);
	
	return(artillery[currentArtilleryIndex]);
}

//---------------------------------------------------------------------------
#define	MAX_TERRAIN_OBJECTS		3000

#define OLD_FILE_SIZE	2200

void GameObjectManager::countTerrainObjects (PacketFile* terrainFile, long firstHandle) 
{
	int i;
	int packet = terrainFile->getCurrentPacket();
	int size = terrainFile->getPacketSize();
	MemoryPtr pBuffer = new unsigned char[size];

#ifdef _DEBUG	
	int bytesRead = 
#endif
		terrainFile->readPacket( packet, pBuffer );
	gosASSERT( bytesRead == size );
	
	File* terrainObjectFile = new File;
	terrainObjectFile->open( (char*)pBuffer, size );
	
	totalObjCount = terrainObjectFile->readLong();

	if (totalObjCount)
	{
		objData = (ObjDataLoader *)systemHeap->Malloc(sizeof(ObjDataLoader) * totalObjCount);
		memset(objData,0,sizeof(ObjDataLoader) * totalObjCount);
	}
	else
	{
		objData = NULL;
	}
	
	ObjDataLoader *data = objData;
	for ( i = 0; i < totalObjCount; ++i )
	{
		data->objTypeNum = terrainObjectFile->readLong();
		data->vector.x = terrainObjectFile->readFloat();
		data->vector.y = terrainObjectFile->readFloat();
		data->vector.z = terrainObjectFile->readFloat();

		data->rotation = terrainObjectFile->readFloat();		
		data->damage = terrainObjectFile->readLong();

		data->teamId = terrainObjectFile->readLong();
		data->parentId = terrainObjectFile->readLong();
		
		// padding
		terrainObjectFile->readLong();
		terrainObjectFile->readLong();

		// convert to block and vertex
		long tileCol;
		long tileRow;

		land->worldToTile( data->vector, tileRow, tileCol );

		int blockI = tileCol/20;
		int blockJ = tileRow/20;
		
		data->blockNumber = blockJ * land->blocksMapSide + blockI;
		tileCol -= blockI * 20;
		tileRow -= blockJ * 20;
		
		data->vertexNumber = tileRow * 20 + tileRow;
		countObject(data);
		
		data++;
	}

	// fix up handles
	long curHandle = firstHandle;
	for ( i = 0; i < Terrain::numObjBlocks; ++i )
	{
		Terrain::objBlockInfo[i].firstHandle = curHandle;
		curHandle += Terrain::objBlockInfo[i].numObjects;						
	}

	delete terrainObjectFile;
	delete pBuffer;

}

void GameObjectManager::countObject( ObjDataLoader *data)
{
	ObjectTypePtr objType = objTypeManager->get(data->objTypeNum);
	if (!objType)
		objType = objTypeManager->load(data->objTypeNum);
	
	if (!objType)
		return;
			//Fatal(objDataBlock[i].objTypeNum, " GameObjectManager.countTerrainObjects: bad objType ");
		switch (objType->getObjectClass()) 
		{
			case TERRAINOBJECT:
			case TREE:
				Terrain::objBlockInfo[data->blockNumber].numCollidableObjects++;
				numTerrainObjects++;
				break;
			case TURRET:
				Terrain::objBlockInfo[data->blockNumber].numCollidableObjects++;
				numTurrets++;
				break;
			case GATE:
				Terrain::objBlockInfo[data->blockNumber].numCollidableObjects++;
				numGates++;
				break;
			case BUILDING:
			case TREEBUILDING:
			if (((((BuildingTypePtr)objType)->perimeterAlarmRange > 0.0f) &&
				(((BuildingTypePtr)objType)->perimeterAlarmTimer > 0.0f)) ||
				(((BuildingTypePtr)objType)->lookoutTowerRange > 0.0f) ||
				(((BuildingTypePtr)objType)->sensorRange > 0.0f))
				{
					Terrain::objBlockInfo[data->blockNumber].numCollidableObjects++;
				}
				numBuildings++;
				break;
			case BRIDGE:
				numTerrainObjects++;
				break;
			default:
				Fatal(objType->getObjectClass(), " GameObjectManager.countTerrainObjects: bad object type ");
		}
		Terrain::objBlockInfo[data->blockNumber].numObjects++;
}

//---------------------------------------------------------------------------

inline bool isLandMine (long objTypeNum) 
{
	return false;
}

//---------------------------------------------------------------------------

long GameObjectManager::getSpecificObjects (long objClass, long objSubType, GameObjectPtr* objects, long maxObjects) {
	long i;

	long numValidObjects = 0;
	for (i = 0; i < getNumObjects(); i++) {
		GameObjectPtr obj = objList[i];
		if (obj && (obj->getObjectClass() == objClass) && (obj->getObjectType()->getSubType() == objSubType)) {
			if (numValidObjects == maxObjects)
				Fatal(objClass, " GameObjectManager.getSpecificObjects: too many ");
			objects[numValidObjects++] = obj;
		}
	}
	return(numValidObjects);
}

//---------------------------------------------------------------------------

void GameObjectManager::loadTerrainObjects (PacketFile* terrainFile,
											volatile float& progress, float progressRange ) 
{
	int i;
	long curTerrainObjectIndex = 0;
	long curBuildingIndex = 0;
	long curTurretIndex = 0;
	long curGateIndex = 0;

	//------------------------------------------------------------------
	long* handles = new long[2 * Terrain::numObjBlocks];

	for ( i = 0; i < Terrain::numObjBlocks; ++i )
	{
		handles[2 * i] = Terrain::objBlockInfo[i].firstHandle;
		handles[(2 * i) + 1] = Terrain::objBlockInfo[i].firstHandle + Terrain::objBlockInfo[i].numCollidableObjects;
	}

	ObjDataLoader *data = objData;

	float increment = 0.0f;
	if (totalObjCount)
		increment = progressRange/totalObjCount;
	for ( i = 0; i < totalObjCount; ++i )
	{
		addObject( data, 
			curTerrainObjectIndex, curBuildingIndex, 
			curTurretIndex, curGateIndex, handles[2 * data->blockNumber], 
			handles[(2 * data->blockNumber) + 1] 
			);

		data++;
		progress += increment;
	}

	delete handles;
	handles = NULL;
	
	//Done loading the objects, free the memory holding them!!
	systemHeap->Free(objData);
	objData = NULL;

	//---------------------------------------------------
	// Finally, let's build the control building lists...
	for (i = 0; i < numTurrets; i++) 
	{
		if ((turrets[i]->parentId != 0xffffffff) && (turrets[i]->parentId != 0)) 
		{
			BuildingPtr controlBuilding = (BuildingPtr)ObjectManager->findByCellPosition((turrets[i]->parentId>>16),(turrets[i]->parentId & 0x0000ffff));
			if (controlBuilding && !controlBuilding->getFlag(OBJECT_FLAG_CONTROLBUILDING)) 
			{
				controlBuilding->setFlag(OBJECT_FLAG_CAPTURABLE, true);
				controlBuilding->setFlag(OBJECT_FLAG_CONTROLBUILDING, true);
				controlBuilding->listID = numTurretControls;
				turretControls[numTurretControls++] = controlBuilding;
			}
			else if (!controlBuilding)
			{
				Stuff::Vector3D worldPos;
				if (land)
					land->cellToWorld((turrets[i]->parentId>>16),(turrets[i]->parentId & 0x0000ffff),worldPos);
				PAUSE(("Turret linked to bldg @ R %d, C %d  X:%f Y:%f No Bldg there!",(turrets[i]->parentId>>16),(turrets[i]->parentId & 0x0000ffff),worldPos.x,worldPos.y));
				turrets[i]->parentId = 0xffffffff;
			}
		}
	}

	for (i = 0; i < numGates; i++) 
	{
		if ((gates[i]->parentId != 0xffffffff) && (gates[i]->parentId != 0))
		{
			BuildingPtr controlBuilding = (BuildingPtr)ObjectManager->findByCellPosition((gates[i]->parentId>>16),(gates[i]->parentId & 0x0000ffff));
			if (controlBuilding && !controlBuilding->getFlag(OBJECT_FLAG_CONTROLBUILDING)) 
			{
				controlBuilding->setFlag(OBJECT_FLAG_CONTROLBUILDING, true);
				controlBuilding->listID = numGateControls;
				gateControls[numGateControls++] = controlBuilding;
			}
	   		else if (!controlBuilding)
	   		{
	   			PAUSE(("Gate tried to link to building at Row %d, Col %d.  No Bldg there!",(gates[i]->parentId>>16),(gates[i]->parentId & 0x0000ffff)));
				gates[i]->parentId = 0xffffffff;
	   		}
		}
	}

	//-----------------------------------------------------------------------------------
	// Create list of special buildings.  These buildings will be updated at least once
	// every frame regardless of where they are on the terrain and what is visible.
	// This is because perimeter alarms and lookout buildings and ????? must work even
	// if the player is NOT looking at them!!
	for (i=0;i<numBuildings;i++)
	{
		if (buildings[i]->isSpecialBuilding())
		{
			specialBuildings[numSpecialBuildings++] = buildings[i];
		}
	}
	
 	//--------------------------------------------------------------------
	//Now, lets point every lit building to at least one power generator.
	for (i=0;i<numBuildings;i++)
	{
		if (buildings[i]->isPowerSource())
		{
			powerGenerators[numPowerGenerators++] = buildings[i];
		}
	}
	
	if (numPowerGenerators)
	{
		for (i=0;i<numBuildings;i++)
		{
			Stuff::Vector3D maxDist;
			long generatorIndex = 0;
			maxDist.Subtract(buildings[i]->getPosition(),powerGenerators[0]->getPosition());
			float minDistance = maxDist.GetApproximateLength();
			
			for (long j=1;j<numPowerGenerators;j++)
			{
				maxDist.Subtract(buildings[i]->getPosition(),powerGenerators[j]->getPosition()); 
				float newDistance = maxDist.GetApproximateLength(); 
				
				if (newDistance < minDistance)
				{
					generatorIndex = j;
					minDistance = newDistance;
				}
			}	
			
			buildings[i]->setPowerSupply(powerGenerators[generatorIndex]);
		}
		
		for (i=0;i<numTerrainObjects;i++)
		{
			Stuff::Vector3D maxDist;
			long generatorIndex = 0;
			maxDist.Subtract(terrainObjects[i]->getPosition(),powerGenerators[0]->getPosition());
			float minDistance = maxDist.GetApproximateLength();
			
			for (long j=1;j<numPowerGenerators;j++)
			{
				maxDist.Subtract(terrainObjects[i]->getPosition(),powerGenerators[j]->getPosition()); 
				float newDistance = maxDist.GetApproximateLength(); 
				
				if (newDistance < minDistance)
				{
					generatorIndex = j;
					minDistance = newDistance;
				}
			}	
			
			terrainObjects[i]->setPowerSupply(powerGenerators[generatorIndex]);
		}
	}
}

extern GameObjectFootPrint* tempSpecialAreaFootPrints;
extern long tempNumSpecialAreas;

void GameObjectManager::addObject (ObjDataLoader *objData, long& curTerrainObjectIndex, 
								  long& curBuildingIndex, long& curTurretIndex, long &curGateIndex,
								  long& curCollidableHandle, long& curNonCollidableHandle )
{
	long i;
	// THIS IS NOT COMPLETELY DONE! MAKE SURE ALL TERRAIN OBJ TYPES
	// ARE ACCOUNTED FOR HERE!

	long objTypeNum = objData->objTypeNum;
	if (!isLandMine(objTypeNum)) {
		ObjectTypePtr objType = getObjectType(objTypeNum);
		GameObjectPtr obj = NULL;
		if (!objType)
			return;
			//Fatal();
		
		Stuff::Vector2DOf<long> numbers;
		numbers.x = objData->vertexNumber;
		numbers.y = objData->blockNumber;

		unsigned char realDmg = objData->damage & 0x0f;
		switch (objType->getObjectClass()) {
			case TERRAINOBJECT:
			case TREE:
				obj = getTerrainObject(curTerrainObjectIndex++);
				if (obj)
				{
					((TerrainObjectPtr)obj)->init(true, objType);
					if (realDmg)
						((TerrainObjectPtr)obj)->setDamage(((TerrainObjectTypePtr)objType)->getDamageLevel());
						
					objList[curCollidableHandle] = obj;
					obj->setHandle(curCollidableHandle++);
					
					obj->setExists(true);
				}
				else
				{
					Fatal(curBuildingIndex," No More Trees ");
				}
				break;
			case TURRET:
				obj = getTurret(curTurretIndex++);
				if (obj)
				{
					((TurretPtr)obj)->init(true, objType);
					if (realDmg)
						obj->setDamage(((TurretTypePtr)objType)->getDamageLevel());

					objList[curCollidableHandle] = obj;
					obj->setHandle(curCollidableHandle++);
					obj->setExists(true);
					obj->setParentId(objData->parentId);
					obj->setTeamId(objData->teamId,true);
				}
				else
				{
					Fatal(curTurretIndex," No More Turrets ");
				}
				break;

			case GATE:
				obj = getGate(curGateIndex++);
				if (obj)
				{
					((GatePtr)obj)->init(true, objType);
					if (realDmg)
						obj->setDamage(((GateTypePtr)objType)->getDamageLvl());

					objList[curCollidableHandle] = obj;
					obj->setHandle(curCollidableHandle++);
					obj->setExists(true);
					obj->setParentId(objData->parentId);
					obj->setTeamId(objData->teamId,true);
				}
				else
				{
					Fatal(curGateIndex," No More Gates ");
				}
				break;

			case BUILDING:
			case TREEBUILDING:
				obj = getBuilding(curBuildingIndex++);
				if (obj)
				{
					((BuildingPtr)obj)->init(true, objType);
					if (realDmg)
						obj->setDamage(((BuildingTypePtr)objType)->getDamageLevel());
					((BuildingPtr)obj)->baseTileId = (objData->damage >> 4);
					if (obj->isSpecialBuilding())
					{
						objList[curCollidableHandle] = obj;
						obj->setHandle(curCollidableHandle++);
					}
					else
					{
						objList[curNonCollidableHandle] = obj;
						obj->setHandle(curNonCollidableHandle++);
					}
					
					obj->setExists(true);
					obj->setParentId(objData->parentId);
					obj->setTeamId(objData->teamId,true);
				}
				else
				{
					Fatal(curBuildingIndex," No More Buildings ");
				}
				break;
			case BRIDGE:
				break;
			default:
				Fatal(objType->getObjectClass(), " GameObjectManager.countTerrainObjects: bad object type ");
		}

		if (obj) {
			obj->setTerrainPosition(objData->vector, numbers);
			long cellRow, cellCol;
			obj->getCellPosition(cellRow, cellCol);
			setPartId(obj, cellRow, cellCol);

			//Just keep the designers from hurting themselves
			if (objType->getObjectClass() != TURRET)
				obj->setRotation(objData->rotation);

			if (obj->isTerrainObject()) {
				Stuff::Vector3D pos = objData->vector;
				((TerrainObjectPtr)obj)->calcCellFootprint(pos);
				if (obj->getObjectClass() == BUILDING) {
					if (objType->getSubType() == BUILDING_SUBTYPE_WALL) {
						for (i = 0; i < tempNumSpecialAreas; i++)
							if (tempSpecialAreaFootPrints[i].cellPositionRow == cellRow)
								if (tempSpecialAreaFootPrints[i].cellPositionCol == cellCol) {
									((BuildingPtr)obj)->calcSubAreas(tempSpecialAreaFootPrints[i].numCells, tempSpecialAreaFootPrints[i].cells);
									break;
								}
						((BuildingPtr)obj)->closeSubAreas();
						}
					else if (objType->getSubType() == BUILDING_SUBTYPE_LANDBRIDGE) {
						for (i = 0; i < tempNumSpecialAreas; i++)
							if (tempSpecialAreaFootPrints[i].cellPositionRow == cellRow)
								if (tempSpecialAreaFootPrints[i].cellPositionCol == cellCol) {
									((BuildingPtr)obj)->calcSubAreas(tempSpecialAreaFootPrints[i].numCells, tempSpecialAreaFootPrints[i].cells);
									break;
								}
						((BuildingPtr)obj)->openSubAreas();
					}
					}
				else if (obj->getObjectClass() == GATE) {
					for (i = 0; i < tempNumSpecialAreas; i++)
						if (tempSpecialAreaFootPrints[i].cellPositionRow == cellRow)
							if (tempSpecialAreaFootPrints[i].cellPositionCol == cellCol) {
								((BuildingPtr)obj)->calcSubAreas(tempSpecialAreaFootPrints[i].numCells, tempSpecialAreaFootPrints[i].cells);
								break;
							}
					((GatePtr)obj)->openSubAreas();
					for (i = 0; i < ((GatePtr)obj)->numSubAreas0; i++) {
						GlobalMoveMap[0]->setAreaOwnerWID(((GatePtr)obj)->subAreas0[i], ((GatePtr)obj)->getWatchID());
						GlobalMoveMap[1]->setAreaOwnerWID(((GatePtr)obj)->subAreas1[i], ((GatePtr)obj)->getWatchID());
						if (((GatePtr)obj)->status == OBJECT_STATUS_DESTROYED) {
							GlobalMoveMap[0]->setAreaTeamID(((GatePtr)obj)->subAreas0[i], -1);
							GlobalMoveMap[1]->setAreaTeamID(((GatePtr)obj)->subAreas1[i], -1);
							}
						else {
							GlobalMoveMap[0]->setAreaTeamID(((GatePtr)obj)->subAreas0[i], ((GatePtr)obj)->teamId);
							GlobalMoveMap[1]->setAreaTeamID(((GatePtr)obj)->subAreas1[i], -1);
						}
					}
					((GatePtr)obj)->setTeamId(obj->getTeamId(), true);
					short* curCoord = ((GatePtr)obj)->cellsCovered;
					for (i = 0; i < ((GatePtr)obj)->numCellsCovered; i++) {
						long r = *curCoord++;
						long c = *curCoord++;
						GameMap->setGate(r, c, true);
					}
				}
			}
		}
	}
}

//---------------------------------------------------------------------------

void GameObjectManager::destroy (void) 
{
	//--------------------------------------------------------------
	// Free 'em all up!!
	long i=0;
	char teardownPhase[256];
	// PORT: A1b is now bounded to this function after a live Gemini salvage /
	// repair completion. Name mover deletes individually (the recovered units are
	// the discriminator), but keep bulk world-object pools at category granularity.
	MC2_TeardownCheckpoint("ObjectManager mechs begin");
	if (mechs && maxMechs > 0) 
	{
		for (i = 0; i < maxMechs; i++) 
		{
			if (mechs[i]) {
				sprintf(teardownPhase, "OM mech[%ld] %p '%s' before", i,
					(void*)mechs[i], mechs[i]->getName());
				MC2_TeardownCheckpoint(teardownPhase);
			}
			delete mechs[i];
			mechs[i] = NULL;
		}
	}
	mechs = NULL;
	MC2_TeardownCheckpoint("ObjectManager mechs done");

	//--------------------------------------------------------------
	MC2_TeardownCheckpoint("ObjectManager vehicles begin");
	if (vehicles && maxVehicles > 0) 
	{
		for (i = 0; i < maxVehicles; i++) 
		{
			if (vehicles[i]) {
				sprintf(teardownPhase, "OM vehicle[%ld] %p '%s' before", i,
					(void*)vehicles[i], vehicles[i]->getName());
				MC2_TeardownCheckpoint(teardownPhase);
			}
			delete vehicles[i];
			vehicles[i] = NULL;
		}
	}
	vehicles = NULL;
	MC2_TeardownCheckpoint("ObjectManager vehicles done");

	//--------------------------------------------------------------
	MC2_TeardownCheckpoint("ObjectManager terrain begin");
	if (terrainObjects && numTerrainObjects > 0) 
	{
		for (i = 0; i < numTerrainObjects; i++) 
		{
			delete terrainObjects[i];
			terrainObjects[i] = NULL;
		}
	}
	terrainObjects = NULL;
	MC2_TeardownCheckpoint("ObjectManager terrain done");

	//--------------------------------------------------------------
	MC2_TeardownCheckpoint("ObjectManager buildings begin");
	if (buildings && numBuildings > 0) 
	{
		for (i = 0; i < numBuildings; i++) 
		{
			delete buildings[i];
			buildings[i] = NULL;
		}
	}
	buildings = NULL;
	MC2_TeardownCheckpoint("ObjectManager buildings done");

	//--------------------------------------------------------------
	MC2_TeardownCheckpoint("ObjectManager turrets begin");
	if (turrets && numTurrets > 0)
	{
		for (i = 0; i < numTurrets; i++) 
		{
			delete turrets[i];
			turrets[i] = NULL;
		}
	}
	turrets = NULL;
	MC2_TeardownCheckpoint("ObjectManager turrets done");

	//--------------------------------------------------------------
	MC2_TeardownCheckpoint("ObjectManager gates begin");
	if (gates && numGates > 0)
	{
		for (i = 0; i < numGates; i++) 
		{
			delete gates[i];
			gates[i] = NULL;
		}
	}
	gates = NULL;
	MC2_TeardownCheckpoint("ObjectManager gates done");

	//--------------------------------------------------------------
	MC2_TeardownCheckpoint("ObjectManager weapons begin");
	if (weapons && numWeapons > 0) 
	{
		for (i = 0; i < numWeapons; i++) 
		{
			delete weapons[i];
			weapons[i] = NULL;
		}
	}
	weapons = NULL;
	MC2_TeardownCheckpoint("ObjectManager weapons done");

	//--------------------------------------------------------------
	MC2_TeardownCheckpoint("ObjectManager carnage begin");
	if (carnage && numCarnage > 0) 
	{
		for (i = 0; i < numCarnage; i++) 
		{
			delete carnage[i];
			carnage[i] = NULL;
		}
	}
	carnage = NULL;
	MC2_TeardownCheckpoint("ObjectManager carnage done");

	//--------------------------------------------------------------
	MC2_TeardownCheckpoint("ObjectManager lights begin");
	if (lights && numLights > 0) 
	{
		for (i = 0; i < numLights; i++) 
		{
			delete lights[i];
			lights[i] = NULL;
		}
	}
	lights = NULL;
	MC2_TeardownCheckpoint("ObjectManager lights done");

	//--------------------------------------------------------------
	MC2_TeardownCheckpoint("ObjectManager artillery begin");
	if (artillery && numArtillery > 0) 
	{
		for (i = 0; i < numArtillery; i++) 
		{
			delete artillery[i];
			artillery[i] = NULL;
		}
	}
	artillery = NULL;
	MC2_TeardownCheckpoint("ObjectManager artillery done");

	//--------------------------------------------------------------
	MC2_TeardownCheckpoint("ObjectManager types begin");
	if (objTypeManager) 
	{
		delete objTypeManager;
		objTypeManager = NULL;
	}
	MC2_TeardownCheckpoint("ObjectManager types done");

	MC2_TeardownCheckpoint("ObjectManager collision begin");
	delete collisionSystem;
	collisionSystem = NULL;
	MC2_TeardownCheckpoint("ObjectManager collision done");

	MC2_TeardownCheckpoint("ObjectManager LOS begin");
	systemHeap->Free(moverLineOfSightTable);
	moverLineOfSightTable = NULL;
	MC2_TeardownCheckpoint("ObjectManager LOS done");
}

//---------------------------------------------------------------------------

void GameObjectManager::render (bool terrain, bool movers, bool other) {
	long i;

	//-----------------------------------------------------
	//Set render states as few times as possible.

	if (drawOldWay)
	{
		//--------------------------------
		//Set States for Software Renderer
		if (Environment.Renderer == 3)
		{
			gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
	
			gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
			gos_SetRenderState( gos_State_MonoEnable, 1);
			gos_SetRenderState( gos_State_Perspective, 0);
			gos_SetRenderState( gos_State_Clipping, 1);
			gos_SetRenderState( gos_State_Specular, 0);
			gos_SetRenderState( gos_State_Dither, 0);
			gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
			gos_SetRenderState( gos_State_Filter, gos_FilterNone);
			gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
			gos_SetRenderState( gos_State_ZCompare, 1);
			gos_SetRenderState(	gos_State_ZWrite, 1);
		}
		//--------------------------------
		//Set States for Hardware Renderer	
		else
		{
			gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
			gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
			gos_SetRenderState( gos_State_MonoEnable, 0);
			gos_SetRenderState( gos_State_Perspective, 1);
			gos_SetRenderState( gos_State_Clipping, 1);
			gos_SetRenderState( gos_State_Specular, 1);
			gos_SetRenderState( gos_State_Dither, 1);
			gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
			gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
			gos_SetRenderState( gos_State_ZCompare, 1);
			gos_SetRenderState(	gos_State_ZWrite, 1);
		}
	}

	if (terrain && renderObjects) 
	{
		for (long terrainBlock = 0; terrainBlock < Terrain::numObjBlocks; terrainBlock++) 
		{
			if (Terrain::objBlockInfo[terrainBlock].active) 
			{
				long numObjs = Terrain::objBlockInfo[terrainBlock].numObjects;
				long objIndex = Terrain::objBlockInfo[terrainBlock].firstHandle;
				for (long terrainObj = 0; terrainObj < numObjs; terrainObj++, objIndex++) 
				{
					if (objList[objIndex] &&
						Terrain::objVertexActive[objList[objIndex]->getVertexNum()])
					{
						objList[objIndex]->render();
						if (MaxObjectsDrawn) 
						{
							//-----------------------------------------
							// No more element groups, so stop drawing.
							return;
						}
					}
				}
			}
		}
	}

	if (movers) {
		if (mechs && (numMechs < maxMechs)) 
		{
			// PORT (M8-B45/M8-B46): BOUND BY maxMechs, NOT numMechs.
			//
			// Same root cause as the invisible-units fix in buildMoverLists.
			// GameObjectManager::Load fills mechs[] and vehicles[] across the whole array
			// and never restores numMechs/numVehicles (measured: placed 158/180, counts
			// still 38/29). Every loop bounded by those counts therefore skips the units
			// past them after a save is loaded.
			//
			// The vehicle loops in this file ALREADY use maxVehicles -- retail's own
			// asymmetry -- which is why loaded tanks kept moving and shooting while loaded
			// MECHS were frozen and undrawn: the owner's Cobra and Payback, present on the
			// tac map, selectable, with no model and no response to orders.
			//
			// The counts themselves are deliberately NOT rewritten: newMech uses numMechs as
			// the next free slot, so setting it to what Load placed would report the pool
			// full and break reinforcements. The slots skipped by this wider bound are NULL
			// or non-existent and the guard below already drops them.
			for (i = 0; i < maxMechs; i++)
			{
				if (mechs[i] && mechs[i]->getExists())
					mechs[i]->render();
			}
		}

		if (vehicles) {
			for (i = 0; i < maxVehicles; i++)
				if (vehicles[i] && vehicles[i]->getExists())
					vehicles[i]->render();
		}

#ifdef USE_ELEMENTALS
		if (elementals) {
			for (i = 0; i < numElementals; i++)
				if (elementals[i] && elementals[i]->getExists())
					elementals[i]->render();
		}
#endif
	}

	if (other) {
		//----------------------------------------
		// All other objects should be rendered...
		if (weapons) {
			for (i = 0; i < numWeapons; i++) {
				if (weapons[i] && weapons[i]->getExists())
					weapons[i]->render();
			}
		}

		if (carnage) {
			for (i = 0; i < numCarnage; i++) {
				if (carnage[i] && carnage[i]->getExists())
				{
					carnage[i]->render();
				}
			}
		}

		if (lights) {
			for (i = 0; i < numLights; i++) {
				if (lights[i] && lights[i]->getExists())
					lights[i]->render();
			}
		}

		if (artillery) {
			for (i = 0; i < numArtillery; i++) {
				if (artillery[i] && artillery[i]->getExists())
					artillery[i]->render();
			}
		}
	}
	gos_SetRenderState( gos_State_Fog, 0);
}

//---------------------------------------------------------------------------

void GameObjectManager::renderShadows (bool terrain, bool movers, bool other) {
	long i;

	//-----------------------------------------------------
	//Set render states as few times as possible.

	//--------------------------------
	//Set States for Software Renderer
	if (Environment.Renderer == 3)
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeFlat);
		gos_SetRenderState( gos_State_MonoEnable, 1);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Clipping, 1);
		gos_SetRenderState( gos_State_AlphaTest, 0);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 0);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendDecal);
		gos_SetRenderState( gos_State_Filter, gos_FilterNone);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
		gos_SetRenderState( gos_State_ZCompare, 0);
		gos_SetRenderState(	gos_State_ZWrite, 0);
	}
	//--------------------------------
	//Set States for Hardware Renderer	
	else
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeFlat);
		gos_SetRenderState( gos_State_MonoEnable, 1);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Clipping, 1);
		gos_SetRenderState( gos_State_AlphaTest, 1);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 1);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterNone);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
		gos_SetRenderState( gos_State_ZCompare, 2);
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}

	if (terrain && renderObjects) 
	{
		for (long terrainBlock = 0; terrainBlock < Terrain::numObjBlocks; terrainBlock++) 
		{
			if (Terrain::objBlockInfo[terrainBlock].active) 
			{
				long numObjs = Terrain::objBlockInfo[terrainBlock].numObjects;
				long objIndex = Terrain::objBlockInfo[terrainBlock].firstHandle;
				for (long terrainObj = 0; terrainObj < numObjs; terrainObj++, objIndex++) 
				{
					if (objList[objIndex] &&
						Terrain::objVertexActive[objList[objIndex]->getVertexNum()]) 
					{
						objList[objIndex]->renderShadows();
						if (MaxObjectsDrawn) {
							//-----------------------------------------
							// No more element groups, so stop drawing.
							return;
						}
					}
				}
			}
		}
	}

	if (movers) {
		if (mechs) {
			for (i = 0; i < maxMechs; i++)
				if (mechs[i] && mechs[i]->getExists())
					mechs[i]->renderShadows();
		}

		if (vehicles) {
			for (i = 0; i < maxVehicles; i++)
				if (vehicles[i] && vehicles[i]->getExists())
					vehicles[i]->renderShadows();
		}
#ifdef USE_ELEMENTALS
		if (elementals) {
			for (i = 0; i < numElementals; i++)
				if (elementals[i] && elementals[i]->getExists())
					elementals[i]->renderShadows();
		}
#endif
	}

	if (other) {
		if (carnage) {
			for (i = 0; i < numCarnage; i++) {
				if (carnage[i] && carnage[i]->getExists())
					carnage[i]->renderShadows();
			}
		}
	}

	gos_SetRenderState( gos_State_Fog, 0);
}

//---------------------------------------------------------------------------
#ifdef LAB_ONLY
__int64 MCTimeTerrainObjectsUpdate 	= 0;
__int64 MCTimeMechsUpdate 			= 0;
__int64 MCTimeVehiclesUpdate		= 0;
__int64 MCTimeTurretsUpdate			= 0;

__int64 MCTimeTerrainObjectsTL 		= 0;
__int64 MCTimeMechsTL 				= 0;
__int64 MCTimeVehiclesTL			= 0;
__int64 MCTimeTurretsTL				= 0;

__int64 MCTimeAllElseUpdate			= 0;
__int64 MCTimeCaptureListUpdate		= 0;
extern __int64 MCTimeTransformandLight;
extern __int64 MCTimeAnimationandMatrix;
extern __int64 MCTimePerShapeTransform;

unsigned long bldgCount = 0;
#endif

//---------------------------------------------------------------------------
// PORT (M8-B15): THE BLOCK CENSUS -- the gate the cull census pointed at.
//
// MC2_CULL_CENSUS came back with a hard NEGATIVE over the owner's 2026-08-22
// session: 68,056,426 objects tested, 27,568,655 projections (only 5,369
// degenerate, so the instrument was demonstrably NOT blind), and REJECTED WHILE
// STILL ON SCREEN = 0. The sphere cull is not losing the objects.
//
// It cannot be, because a terrain object is only ever REACHED when its block is
// active -- see the loop below and the four sibling loops in this file. A block
// that is not active means its objects are never updated and never rendered, so
// they never get as far as a cull.
//
// AND BLOCKS ARE MADE ACTIVE ONLY FROM THE TERRAIN VERTEX WINDOW.
// Terrain.cpp:1076 calls setObjBlockActive for vertices in vertexList, and
// MapData::makeLists builds that list as a SQUARE of visibleVerticesPerSide
// vertices CENTRED ON THE CAMERA POSITION and AXIS-ALIGNED TO THE MAP --
// mapdata.cpp:669-682, which reads eye->getPosition() and nothing else. It does
// not know the camera's rotation or its projection angle.
//
// That geometry predicts all three things the owner reported:
//   * ROTATION dependence -- a square reaches visibleVerticesPerSide/2 vertices
//     along the map axes but 1.414x that along its diagonals, so which way you
//     face changes how far objects survive. "Rotating to specific directions
//     renders most objects invisible in an area."
//   * ANGLE dependence -- a lower angle looks further from the camera POSITION,
//     which is what the window is centred on, so more of the view falls outside
//     it. At 160 vertices and 128 units each the axis half-reach is 10,240.
//   * POSITION dependence -- "only on the left side of the map".
//
// THIS COUNTS IT INSTEAD OF ARGUING IT: every block skipped for being inactive,
// and how many of those hold an object that still projects ONTO THE SCREEN in
// front of the camera. It carries the same control the cull census needed --
// degenerate 0,0 projections are counted separately and never believed.
//
// CAN COME BACK NEGATIVE: if skipped-but-on-screen is 0 while objects are
// visibly missing, the window is innocent too and the loss is further down
// still, in the appearance update or the draw call.
//
// SILENT unless MC2_BLOCK_CENSUS is non-zero.
//---------------------------------------------------------------------------
static bool  MC2_BlkOn        = false;
static bool  MC2_BlkChecked   = false;
static long  MC2_BlkFrames    = 0;
static long  MC2_BlkSkipped   = 0;   // inactive blocks that hold objects
static long  MC2_BlkProjected = 0;
static long  MC2_BlkDegenerate= 0;
static long  MC2_BlkOnScreen  = 0;   // skipped AND still on screen
static long  MC2_BlkWorstFrame= 0;
static long  MC2_BlkWorstCount= 0;
static long  MC2_BlkWindowSide = 0;   // captured DURING the run
static float MC2_BlkCamAngle = 0.0f, MC2_BlkCamRot = 0.0f;
static float MC2_BlkCamX = 0.0f, MC2_BlkCamY = 0.0f;
static char MC2_BlkMission[96] = "<not captured>";
static long MC2_VtxTested   = 0;
static long MC2_VtxFailed   = 0;
static long MC2_VtxOnScreen = 0;
static long MC2_VtxDegen    = 0;

static void MC2_BlockCensusSummary (void)
{
	if (!MC2_BlkOn)
		return;
	printf("[block] frames=%ld  inactive blocks holding objects=%ld\n",
	       MC2_BlkFrames, MC2_BlkSkipped);
	printf("[block] projections=%ld degenerate=%ld\n",
	       MC2_BlkProjected, MC2_BlkDegenerate);
	if (MC2_BlkProjected > 0 && MC2_BlkDegenerate == MC2_BlkProjected)
	{
		printf("[block] INSTRUMENT BLIND: every projection collapsed to 0,0.\n");
		fflush(stdout);
		return;
	}
	printf("[block] SKIPPED WHILE STILL ON SCREEN = %ld  %s\n",
	       MC2_BlkOnScreen,
	       MC2_BlkOnScreen > 0
	           ? "<-- the terrain window is dropping visible objects"
	           : "(none -- the window is not the cause either; look further down)");
	printf("[block] worst frame=%ld with %ld on-screen objects skipped\n",
	       MC2_BlkWorstFrame, MC2_BlkWorstCount);
	printf("[block] VERTEX GATE: tested=%ld failed=%ld degenerate=%ld\n",
	       MC2_VtxTested, MC2_VtxFailed, MC2_VtxDegen);
	printf("[block] VERTEX GATE DROPPED WHILE ON SCREEN = %ld  %s\n",
	       MC2_VtxOnScreen,
	       MC2_VtxOnScreen > 0
	           ? "<-- the per-vertex gate is dropping visible objects"
	           : "(none -- that gate is innocent too)");
	// Captured DURING the run, not read here: Terrain::destroy zeroes
	// visibleVerticesPerSide, so reading it at exit reported a 0-wide window.
	printf("[block] window: %ld vertices per side x %.1f units = %.0f half-reach "
	       "on the map axes, %.0f on the diagonals\n",
	       MC2_BlkWindowSide, Terrain::worldUnitsPerVertex,
	       (MC2_BlkWindowSide * 0.5f) * Terrain::worldUnitsPerVertex,
	       (MC2_BlkWindowSide * 0.5f) * Terrain::worldUnitsPerVertex * 1.41421f);
	printf("[block] last camera seen: angle=%.3f rotation=%.3f pos %.1f,%.1f\n",
	       MC2_BlkCamAngle, MC2_BlkCamRot, MC2_BlkCamX, MC2_BlkCamY);
	fflush(stdout);
	// Name the MAP. A headless repro of the owner camera was once run against
	// mc2_01 while they were playing a different mission, so the coordinates
	// landed in open sea and every gate read innocent. A census that does not
	// say which map it measured can be read against the wrong one.
	printf("[block] mission: %s\n",
	       MC2_BlkMission);
}

static inline bool MC2_BlockCensusOn (void)
{
	if (!MC2_BlkChecked)
	{
		MC2_BlkChecked = true;
		const char* v = getenv("MC2_BLOCK_CENSUS");
		MC2_BlkOn = v && *v && strcmp(v, "0") != 0;
		if (MC2_BlkOn)
		{
			atexit(MC2_BlockCensusSummary);
			printf("[block] armed -- counting objects skipped because their terrain "
			       "block is outside the camera-centred vertex window\n");
			fflush(stdout);
		}
	}
	return MC2_BlkOn;
}

static long MC2_BlkThisFrame = 0;

// PORT (M8-B15): one-shot roster dump, so a headless repro can be AIMED at a
// place the owner describes in words instead of by guessing coordinates.
// Owner 2026-08-22: "two Anubis mechs standing near a small walled concrete
// outpost with a sensor tower and resource building", top left of mc2_06.
// MC2_MOVER_DUMP=<tick> prints every mover once at that tick.
static void MC2_MoverDump (void)
{
	static long wanted = -1;
	static bool done = false;
	if (wanted < 0)
	{
		const char* v = getenv("MC2_MOVER_DUMP");
		wanted = (v && *v) ? atol(v) : 0;
	}
	if (wanted <= 0 || done || turn < wanted || !ObjectManager)
		return;
	done = true;

	const long n = ObjectManager->getNumMovers();
	printf("[roster] %ld movers at turn %ld\n", n, turn);
	for (long i = 0; i < n; i++)
	{
		MoverPtr m = ObjectManager->getMover(i);
		if (!m) continue;
		Stuff::Vector3D p = m->getPosition();
		printf("[roster]   %-3ld %-20s team=%-3ld exists=%d  pos %.1f,%.1f,%.1f\n",
		       i, m->getName() ? m->getName() : "<none>",
		       (long)m->getTeamId(), (int)m->getExists(),
		       p.x, p.y, p.z);
	}

	// Terrain objects too -- the owner describes places by their BUILDINGS
	// ("a small walled concrete outpost with a sensor tower and resource
	// building"), and trees would drown the list, so only named non-tree
	// objects are printed.
	{
		long printed = 0;
		for (long b = 0; b < Terrain::numObjBlocks && printed < 40000; b++)
		{
			const long n2 = Terrain::objBlockInfo[b].numObjects;
			long idx = Terrain::objBlockInfo[b].firstHandle;
			for (long k = 0; k < n2 && printed < 40000; k++, idx++)
			{
				GameObjectPtr o = ObjectManager->getObjBlockObject(b, k);
				if (!o || !o->getExists()) continue;
				const char* nm = o->getName();
				if (!nm || !*nm) continue;
				if (strstr(nm, "ree") || strstr(nm, "REE")) continue;
				Stuff::Vector3D q = o->getPosition();
				// PROJECTION SANITY CHECK. Every "was it on screen" verdict in the
				// cull and block censuses rests on Camera::projectZ, and
				// Camera::screenResolution has been measured disagreeing with
				// Environment.screenWidth/Height. If projectZ says a building is off
				// screen while the captured frame plainly shows it, those censuses
				// have been under-reporting and their zeros mean nothing.
				Stuff::Vector4D projCheck(-10000.0f, -10000.0f, -10000.0f, -10000.0f);
				Stuff::Vector3D qp = q;
				const bool inRect = eye ? eye->projectZ(qp, projCheck) : false;
				printf("[roster] bldg %-24s pos %.1f,%.1f  screen %.0f,%.0f z=%.3f inRect=%d\n",
				       nm, q.x, q.y, projCheck.x, projCheck.y, projCheck.z, (int)inRect);
				printed++;
			}
		}
		printf("[roster] printed %ld named non-tree terrain objects\n", printed);
	}
	fflush(stdout);
}

static void MC2_BlockCensusFrame (void)
{
	MC2_MoverDump();
	if (!MC2_BlockCensusOn())
		return;
	if (MC2_BlkThisFrame > MC2_BlkWorstCount)
	{
		MC2_BlkWorstCount = MC2_BlkThisFrame;
		MC2_BlkWorstFrame = turn;
	}
	MC2_BlkThisFrame = 0;
	MC2_BlkFrames++;

	// Snapshot what the window and camera actually were this frame. Reading
	// visibleVerticesPerSide at exit reported 0 -- Terrain::destroy zeroes it.
	MC2_BlkWindowSide = Terrain::visibleVerticesPerSide;
	if (eye)
	{
		MC2_BlkCamAngle = eye->getProjectionAngle();
		MC2_BlkCamRot   = eye->getCameraRotation();
		Stuff::Vector3D cp = eye->getPosition();
		MC2_BlkCamX = cp.x;
		MC2_BlkCamY = cp.y;
	}
	if (mission && mission->getMissionFileName() && *mission->getMissionFileName())
	{
		strncpy(MC2_BlkMission, mission->getMissionFileName(),
		        sizeof(MC2_BlkMission) - 1);
		MC2_BlkMission[sizeof(MC2_BlkMission) - 1] = 0;
	}
}

// The per-vertex gate. Counts objects whose BLOCK passed but whose own
// terrain vertex did not, and how many of those still project on screen.

static void MC2_VertexGateNote (GameObjectPtr obj)
{
	if (!MC2_BlkOn || !eye || !obj || !Terrain::objVertexActive)
		return;
	if (!obj->getExists())
		return;

	MC2_VtxTested++;
	const long vn = obj->getVertexNum();
	if (vn >= 0 && vn < (Terrain::realVerticesMapSide * Terrain::realVerticesMapSide)
	    && Terrain::objVertexActive[vn])
		return;

	MC2_VtxFailed++;

	Stuff::Vector3D pos = obj->getPosition();
	Stuff::Vector4D sp(-10000.0f, -10000.0f, -10000.0f, -10000.0f);
	const bool inRect = eye->projectZ(pos, sp);
	if (sp.x == 0.0f && sp.y == 0.0f) { MC2_VtxDegen++; return; }
	if (!inRect || sp.z <= 0.0f) return;

	MC2_VtxOnScreen++;
	if (MC2_VtxOnScreen <= 15 || (MC2_VtxOnScreen % 5000) == 0)
	{
		fprintf(stderr,
			"[block] VERTEX GATE dropped an ON-SCREEN object #%ld turn=%ld "
			"vertex=%ld screen %.0f,%.0f world %.1f,%.1f\n",
			MC2_VtxOnScreen, turn, vn, sp.x, sp.y, pos.x, pos.y);
		fflush(stderr);
	}
}

static void MC2_BlockCensusSkipped (long blockNum, GameObjectPtr* objList)
{
	if (!MC2_BlkOn || !eye || !objList)
		return;

	const long numObjs = Terrain::objBlockInfo[blockNum].numObjects;
	if (numObjs <= 0)
		return;
	MC2_BlkSkipped++;

	// Project a handful, not all of them -- a full map has tens of thousands.
	const long sample = numObjs < 4 ? numObjs : 4;
	long objIndex = Terrain::objBlockInfo[blockNum].firstHandle;
	for (long i = 0; i < sample; i++, objIndex++)
	{
		GameObjectPtr obj = objList[objIndex];
		if (!obj || !obj->getExists())
			continue;

		Stuff::Vector3D pos = obj->getPosition();
		Stuff::Vector4D sp(-10000.0f, -10000.0f, -10000.0f, -10000.0f);
		const bool inRect = eye->projectZ(pos, sp);

		MC2_BlkProjected++;
		if (sp.x == 0.0f && sp.y == 0.0f) { MC2_BlkDegenerate++; continue; }
		if (!inRect || sp.z <= 0.0f) continue;

		MC2_BlkOnScreen++;
		MC2_BlkThisFrame++;
		if (MC2_BlkOnScreen <= 15 || (MC2_BlkOnScreen % 2000) == 0)
		{
			fprintf(stderr,
				"[block] SKIPPED BUT ON SCREEN #%ld turn=%ld block=%ld "
				"screen %.0f,%.0f  world %.1f,%.1f\n",
				MC2_BlkOnScreen, turn, blockNum, sp.x, sp.y, pos.x, pos.y);
			fflush(stderr);
		}
	}
}

// PORT (perf, 2026-08-22): split the OBJECTS phase, which is 67.9% of
// Mission::update at the heavy camera. MC2_XFORM_CENSUS showed only 41% of it
// is MultiTransformShape, so the larger half is elsewhere in these three loops
// and optimising the transform alone would have missed it.
// SILENT unless MC2_OBJ_SPLIT is non-zero.
#define MC2_MAX_OBJECT_WORKERS_PROBE 8
static bool   MC2_ObjSplitOn = false, MC2_ObjSplitChecked = false;
static double MC2_ObjTerrainMs = 0.0, MC2_ObjMoverMs = 0.0, MC2_ObjOtherMs = 0.0;
static double MC2_ObjTextureRefreshMs = 0.0;

// PORT (M8-B18): split the TERRAIN-OBJECT phase itself. Re-profiled 2026-08-23,
// that phase is 2.61 ms of an 8.29 ms frame with four workers and 4.15 ms
// serial, so an ideal 4-way split would be ~1.04 ms and roughly 1.57 ms is
// sitting in its serial remainder. 144 fps needs 1.35 ms found, so WHICH
// remainder decides the next move -- and guessing which of these four is the
// big one is exactly how the object path reached the allocator last time.
static double MC2_ObjGatherMs = 0.0;        // walking blocks, building the list
static double MC2_ObjSerialUpdateMs = 0.0;  // object->update(), transforms deferred
static double MC2_ObjParallelMs = 0.0;      // the worker team, wall clock

// PORT (M8-B18): WHY is the transform only getting 2.46x out of four workers?
// Two explanations fit that number and they call for opposite fixes:
//   (a) LOAD IMBALANCE -- the workers finish at very different times, so the
//       busy totals differ sharply and their SUM is close to the serial cost.
//       The fix is scheduling.
//   (b) CONTENTION -- each worker is individually slower than serial code
//       doing the same work, so the SUM is much larger than the serial cost.
//       The fix is data layout, and scheduling would achieve nothing.
// Each worker writes only its own slot, so these need no synchronisation.
static double MC2_ObjWorkerBusyMs[MC2_MAX_OBJECT_WORKERS_PROBE] = {0};
static long   MC2_ObjWorkerObjects[MC2_MAX_OBJECT_WORKERS_PROBE] = {0};
static long   MC2_ObjWorkerCount = 0;

static void MC2_ObjSplitSummary (void)
{
	if (!MC2_ObjSplitOn) return;
	const double t = MC2_ObjTerrainMs + MC2_ObjMoverMs + MC2_ObjOtherMs;
	printf("[objsplit] terrain objects %.0f ms (%.1f%%)  movers %.0f ms (%.1f%%)  other %.0f ms (%.1f%%)\n",
	       MC2_ObjTerrainMs, t > 0 ? 100.0 * MC2_ObjTerrainMs / t : 0.0,
	       MC2_ObjMoverMs,   t > 0 ? 100.0 * MC2_ObjMoverMs   / t : 0.0,
	       MC2_ObjOtherMs,   t > 0 ? 100.0 * MC2_ObjOtherMs   / t : 0.0);
	printf("[objsplit] parallel serial texture refresh %.0f ms\n", MC2_ObjTextureRefreshMs);
	const double inner = MC2_ObjGatherMs + MC2_ObjSerialUpdateMs
	                   + MC2_ObjParallelMs + MC2_ObjTextureRefreshMs;
	if (inner > 0.0)
		printf("[objsplit] terrain phase inside: gather %.0f ms (%.1f%%)  "
		       "serial update %.0f ms (%.1f%%)  parallel transform %.0f ms (%.1f%%)  "
		       "texture refresh %.0f ms (%.1f%%)\n",
		       MC2_ObjGatherMs,         100.0 * MC2_ObjGatherMs / inner,
		       MC2_ObjSerialUpdateMs,   100.0 * MC2_ObjSerialUpdateMs / inner,
		       MC2_ObjParallelMs,       100.0 * MC2_ObjParallelMs / inner,
		       MC2_ObjTextureRefreshMs, 100.0 * MC2_ObjTextureRefreshMs / inner);
	if (MC2_ObjWorkerCount > 0)
	{
		double busySum = 0.0, busyMax = 0.0;
		for (long w = 0; w < MC2_ObjWorkerCount; ++w)
		{
			busySum += MC2_ObjWorkerBusyMs[w];
			if (MC2_ObjWorkerBusyMs[w] > busyMax) busyMax = MC2_ObjWorkerBusyMs[w];
			printf("[objsplit]   worker %ld busy %.0f ms  objects %ld\n",
			       w, MC2_ObjWorkerBusyMs[w], MC2_ObjWorkerObjects[w]);
		}
		// busySum vs the parallel wall clock separates imbalance from contention.
		printf("[objsplit]   worker busy sum %.0f ms  busiest %.0f ms  "
		       "wall %.0f ms  -> imbalance %.2fx, idle %.0f ms\n",
		       busySum, busyMax, MC2_ObjParallelMs,
		       busySum > 0.0 ? (busyMax * MC2_ObjWorkerCount) / busySum : 0.0,
		       (MC2_ObjParallelMs * MC2_ObjWorkerCount) - busySum);
	}
	fflush(stdout);
}

static inline bool MC2_ObjSplit (void)
{
	if (!MC2_ObjSplitChecked)
	{
		MC2_ObjSplitChecked = true;
		const char* e = getenv("MC2_OBJ_SPLIT");
		MC2_ObjSplitOn = e && *e && strcmp(e, "0") != 0;
		if (MC2_ObjSplitOn) atexit(MC2_ObjSplitSummary);
	}
	return MC2_ObjSplitOn;
}

typedef std::chrono::steady_clock MC2ObjClock;

// PORT (perf): opt-in only. Zero and invalid values preserve the original
// single-threaded loop below. The cap matches the worker-pool table in TGL.
static long MC2_ParallelObjectWorkers (void)
{
	static bool checked = false;
	static long workers = 0;
	if (!checked)
	{
		checked = true;
		const char* e = getenv("MC2_PARALLEL_OBJECTS");
		if (e && *e)
		{
			char* end = NULL;
			const long requested = strtol(e, &end, 10);
			if (end != e && *end == '\0' && requested > 0)
				workers = min(requested, 8L);
		}
	}
	return workers;
}

static bool MC2_WorkerAllocationCensus (void)
{
	static const bool enabled = []() {
		const char* e = getenv("MC2_WORKER_ALLOC_CENSUS");
		return e && *e && strcmp(e, "0") != 0;
	}();
	return enabled;
}

static bool MC2_WorkerAllocationSelfTest (void)
{
	static const bool enabled = []() {
		const char* e = getenv("MC2_WORKER_ALLOC_SELFTEST");
		return e && *e && strcmp(e, "0") != 0;
	}();
	return enabled;
}

static std::atomic<bool> MC2_WorkerAllocationSelfTestPending(true);

class MC2ObjectWorkerTeam
{
	long workerCount;
	std::vector<std::thread> helpers;
	std::mutex mutex;
	std::condition_variable startCondition;
	std::condition_variable doneCondition;
	std::vector<GameObjectPtr>* objects;
	unsigned long generation;
	long remaining;
	std::atomic<size_t> nextObject;

	void process (long worker)
	{
		const bool timeMe = MC2_ObjSplit() && worker < MC2_MAX_OBJECT_WORKERS_PROBE;
		const MC2ObjClock::time_point busyT0 =
			timeMe ? MC2ObjClock::now() : MC2ObjClock::time_point();
		long myObjects = 0;
		MC2_SetWorkerAllocationProbe(true);
		if (MC2_WorkerAllocationSelfTest() &&
			MC2_WorkerAllocationSelfTestPending.exchange(false, std::memory_order_relaxed))
		{
			void* positiveControl = gos_Malloc(1, NULL);
			gos_Free(positiveControl);
		}
		const size_t chunkSize = 8;
		for (;;)
		{
			const size_t begin = nextObject.fetch_add(chunkSize, std::memory_order_relaxed);
			if (begin >= objects->size())
				break;
			const size_t end = (begin + chunkSize < objects->size()) ? begin + chunkSize : objects->size();
			for (size_t objectIndex = begin; objectIndex < end; ++objectIndex)
			{
				GameObjectPtr object = (*objects)[objectIndex];
				AppearancePtr appearance = object->getAppearance();
				if (appearance)
					appearance->parallelTransform();
				++myObjects;
			}
		}
		MC2_SetWorkerAllocationProbe(false);
		if (timeMe)
		{
			MC2_ObjWorkerBusyMs[worker] += std::chrono::duration<double, std::milli>(
				MC2ObjClock::now() - busyT0).count();
			MC2_ObjWorkerObjects[worker] += myObjects;
			if (worker >= MC2_ObjWorkerCount) MC2_ObjWorkerCount = worker + 1;
		}
	}

	void helperLoop (long worker)
	{
		TG_SelectParallelPool(worker);
		unsigned long seenGeneration = 0;
		for (;;)
		{
			std::unique_lock<std::mutex> lock(mutex);
			startCondition.wait(lock, [&]() { return generation != seenGeneration; });
			seenGeneration = generation;
			lock.unlock();
			process(worker);
			lock.lock();
			if (--remaining == 0)
				doneCondition.notify_one();
		}
	}

	public:
		MC2ObjectWorkerTeam (long count)
			: workerCount(count), objects(NULL), generation(0), remaining(0), nextObject(0)
		{
			helpers.reserve(workerCount > 1 ? workerCount - 1 : 0);
			for (long worker = 1; worker < workerCount; ++worker)
				helpers.emplace_back(&MC2ObjectWorkerTeam::helperLoop, this, worker);
		}

		void run (std::vector<GameObjectPtr>& activeObjects)
		{
			MC2_ResetWorkerAllocationCount();
			{
				std::lock_guard<std::mutex> lock(mutex);
				objects = &activeObjects;
				nextObject.store(0, std::memory_order_relaxed);
				remaining = (long)helpers.size();
				++generation;
			}
			startCondition.notify_all();
			MC2_TGLParallelWorker = true;
			process(0);
			MC2_TGLParallelWorker = false;
			if (!helpers.empty())
			{
				std::unique_lock<std::mutex> lock(mutex);
				doneCondition.wait(lock, [&]() { return remaining == 0; });
			}
			if (MC2_WorkerAllocationCensus())
			{
				printf("[worker-alloc] gos_Malloc calls=%llu\n", MC2_GetWorkerAllocationCount());
				fflush(stdout);
			}
		}
};

static MC2ObjectWorkerTeam* MC2_ObjectWorkerTeam (long workerCount)
{
	// The environment is parsed once, so the count cannot change in-process.
	// Intentionally process-lifetime: OS teardown releases waiting helper threads.
	static MC2ObjectWorkerTeam* team = NULL;
	if (!team)
		team = new MC2ObjectWorkerTeam(workerCount);
	return team;
}

void GameObjectManager::update (bool terrain, bool movers, bool other)
{
	long i;
	//----------------------------
	// Now, update game objects...
	#ifdef LAB_ONLY
	__int64 x;
	x=GetCycles();
	#endif
	
	updateCaptureList();
	
	#ifdef LAB_ONLY
	x=GetCycles()-x;
	MCTimeCaptureListUpdate = x;
	
	x=GetCycles(); 
	#endif
	
	const bool mc2ObjSplit = MC2_ObjSplit();
	MC2ObjClock::time_point mc2ObjT;
	if (mc2ObjSplit) mc2ObjT = MC2ObjClock::now();
 	if (terrain && renderObjects) 
	{
		#ifdef LAB_ONLY
		bldgCount = 0;
		#endif
		
		//First Update all of the Special Buildings.
		// They will mark themselves updated and not re-update below.
		for (long spBuilding = 0; spBuilding < numSpecialBuildings;spBuilding++)
		{
			if (specialBuildings[spBuilding] && specialBuildings[spBuilding]->getExists()) 
			{
			#ifdef LAB_ONLY
				bldgCount++;
				MCTimeAnimationandMatrix =
				MCTimePerShapeTransform =
				MCTimeTransformandLight = 0;
			#endif
				if (!specialBuildings[spBuilding]->update()) 
				{
					//-----------------------------------------
					// Update failed, so it no longer exists...
					specialBuildings[spBuilding]->setExists(false);
				}
			#ifdef LAB_ONLY
				MCTimeTerrainObjectsTL += MCTimeTransformandLight;
			#endif
			}
		}

		//Then update all of the gates.
		// They too will mark themselves and not re-update.
		// MUST update every frame or they don't open!!
		for (long nGates = 0;nGates < numGates;nGates++)
		{
			if (gates[nGates] && gates[nGates]->getExists()) 
			{
			#ifdef LAB_ONLY
				bldgCount++;
				MCTimeAnimationandMatrix =
				MCTimePerShapeTransform =
				MCTimeTransformandLight = 0;
			#endif
				if (!gates[nGates]->update()) 
				{
					//-----------------------------------------
					// Update failed, so it no longer exists...
					gates[nGates]->setExists(false);
				}
			#ifdef LAB_ONLY
				MCTimeTerrainObjectsTL += MCTimeTransformandLight;
			#endif
			}
		}
		
		MC2_BlockCensusFrame();

		const long mc2ParallelWorkers = MC2_ParallelObjectWorkers();
		// Turns 0-2 deliberately touch every terrain object and may lazy-load shape
		// and texture assets through shared retail-era loaders. Warm them serially;
		// the normal active-block population is parallel from turn 3 onward.
		if (mc2ParallelWorkers > 0 && turn >= 3 && TG_PrepareParallelPools(mc2ParallelWorkers))
		{
			std::vector<GameObjectPtr> activeTerrainObjects;
			activeTerrainObjects.reserve(numTerrainObjects);
			for (long terrainBlock = 0; terrainBlock < Terrain::numObjBlocks; terrainBlock++)
			{
				if (!(Terrain::objBlockInfo[terrainBlock].active || (turn < 3)))
					MC2_BlockCensusSkipped(terrainBlock, &objList[0]);
				if (Terrain::objBlockInfo[terrainBlock].active || (turn < 3))
				{
					long numObjs = Terrain::objBlockInfo[terrainBlock].numObjects;
					long objIndex = Terrain::objBlockInfo[terrainBlock].firstHandle;
					for (long terrainObj = 0; terrainObj < numObjs; terrainObj++, objIndex++)
					{
						MC2_VertexGateNote(objList[objIndex]);
						if (objList[objIndex] &&
							(Terrain::objVertexActive[objList[objIndex]->getVertexNum()] || (turn < 3)) &&
							objList[objIndex]->getExists())
							activeTerrainObjects.push_back(objList[objIndex]);
					}
				}
			}

			// PORT (M8-B18): the gather above is the first of four sub-phases. Time
			// each one so the next parallelisation step is chosen on a measurement.
			if (mc2ObjSplit)
				MC2_ObjGatherMs += std::chrono::duration<double, std::milli>(
					MC2ObjClock::now() - mc2ObjT).count();

			// Keep gameplay, animation, culling and asset preparation in original
			// order on the main thread. Only the prepared TGL transform crosses
			// the worker barrier.
			MC2ObjClock::time_point mc2SerialUpdate;
			if (mc2ObjSplit) mc2SerialUpdate = MC2ObjClock::now();
			MC2_DeferTerrainTransforms = true;
			for (size_t objectIndex = 0; objectIndex < activeTerrainObjects.size(); ++objectIndex)
			{
				GameObjectPtr object = activeTerrainObjects[objectIndex];
				if (!object->update())
					object->setExists(false);
			}
			MC2_DeferTerrainTransforms = false;
			if (mc2ObjSplit)
				MC2_ObjSerialUpdateMs += std::chrono::duration<double, std::milli>(
					MC2ObjClock::now() - mc2SerialUpdate).count();

			MC2ObjClock::time_point mc2ParallelPhase;
			if (mc2ObjSplit) mc2ParallelPhase = MC2ObjClock::now();
			TG_ResetParallelPools();
			MC2_ObjectWorkerTeam(mc2ParallelWorkers)->run(activeTerrainObjects);
			if (mc2ObjSplit)
				MC2_ObjParallelMs += std::chrono::duration<double, std::milli>(
					MC2ObjClock::now() - mc2ParallelPhase).count();
			// Texture handles are consumed by the later serial render pass, not by
			// geometry transforms. Refresh only the appearances culling kept visible.
			MC2ObjClock::time_point mc2TextureRefresh;
			if (mc2ObjSplit) mc2TextureRefresh = MC2ObjClock::now();
			for (size_t objectIndex = 0; objectIndex < activeTerrainObjects.size(); ++objectIndex)
			{
				AppearancePtr appearance = activeTerrainObjects[objectIndex]->getAppearance();
				if (appearance && appearance->canBeSeen())
					appearance->prepareParallelTextures();
			}
			if (mc2ObjSplit)
				MC2_ObjTextureRefreshMs += std::chrono::duration<double, std::milli>(MC2ObjClock::now() - mc2TextureRefresh).count();
		}
		else
		{
			for (long terrainBlock = 0; terrainBlock < Terrain::numObjBlocks; terrainBlock++)
			{
				if (!(Terrain::objBlockInfo[terrainBlock].active || (turn < 3)))
					MC2_BlockCensusSkipped(terrainBlock, &objList[0]);

				if (Terrain::objBlockInfo[terrainBlock].active || (turn < 3))
				{
					long numObjs = Terrain::objBlockInfo[terrainBlock].numObjects;
					long objIndex = Terrain::objBlockInfo[terrainBlock].firstHandle;
					for (long terrainObj = 0; terrainObj < numObjs; terrainObj++,objIndex++)
					{
					// PORT (M8-B15): the SECOND gate, and the tighter one. The block gate
					// above is 20x20 vertices wide; this one is per-vertex, so an object
					// dies if ITS OWN terrain vertex failed the terrain cull even when its
					// block passed. Measured here because the block census came back innocent.
					MC2_VertexGateNote(objList[objIndex]);

					if (objList[objIndex] && 
						(Terrain::objVertexActive[objList[objIndex]->getVertexNum()] || (turn < 3)) && 
						objList[objIndex]->getExists())
					{
			#ifdef LAB_ONLY
				bldgCount++;
				
				MCTimeAnimationandMatrix =
				MCTimePerShapeTransform =
				MCTimeTransformandLight = 0;
			#endif
						
						if (!objList[objIndex]->update()) 
						{
							//-----------------------------------------
							// Update failed, so it no longer exists...
							objList[objIndex]->setExists(false);
						}
						
			#ifdef LAB_ONLY
				MCTimeTerrainObjectsTL += MCTimeTransformandLight;
			#endif
					}
					}
				}
			}
		}
	}
	if (mc2ObjSplit) MC2_ObjTerrainMs += std::chrono::duration<double, std::milli>(MC2ObjClock::now() - mc2ObjT).count();

	#ifdef LAB_ONLY
	x=GetCycles()-x;
	MCTimeTerrainObjectsUpdate = x;
	#endif
	
	if (mc2ObjSplit) mc2ObjT = MC2ObjClock::now();
 	if (movers) {
		static MoverPtr removeList[MAX_MOVERS];
		long numRemoved = 0;
		
		#ifdef LAB_ONLY
		x=GetCycles();
		#endif
		
		if (mechs)
		{
			for (i = 0; i < maxMechs; i++) 
			{
				MoverPtr mover = mechs[i];
				if (mover && mover->getExists()) 
				{
			#ifdef LAB_ONLY
				MCTimeAnimationandMatrix =
				MCTimePerShapeTransform =
				MCTimeTransformandLight = 0;
			#endif
					if (!mover->update())
						mover->setExists(false);
			#ifdef LAB_ONLY
				MCTimeMechsTL += MCTimeTransformandLight;
			#endif
					if (mover->getFlag(OBJECT_FLAG_REMOVED))
						removeList[numRemoved++] = mover;
				}
			}
		}

		#ifdef LAB_ONLY
		x=GetCycles()-x;
		MCTimeMechsUpdate = x;
		
		x=GetCycles(); 
		#endif
	
		if (vehicles)
		{
			for (i = 0; i < maxVehicles; i++) 
			{
				MoverPtr mover = vehicles[i];
				if (mover && mover->getExists()) 
				{
			#ifdef LAB_ONLY
				MCTimeAnimationandMatrix =
				MCTimePerShapeTransform =
				MCTimeTransformandLight = 0;
			#endif
					if (!mover->update())
						mover->setExists(false);
			#ifdef LAB_ONLY
				MCTimeVehiclesTL += MCTimeTransformandLight;
			#endif
					if (mover->getFlag(OBJECT_FLAG_REMOVED))
						removeList[numRemoved++] = mover;
				}
			}
		}
			
		#ifdef LAB_ONLY
		x=GetCycles()-x;
		MCTimeVehiclesUpdate = x;
		#endif
		
		for (i = 0; i < numRemoved; i++)
			mission->removeMover(removeList[i]);
	}
	if (mc2ObjSplit) MC2_ObjMoverMs += std::chrono::duration<double, std::milli>(MC2ObjClock::now() - mc2ObjT).count();

	if (mc2ObjSplit) mc2ObjT = MC2ObjClock::now();
	if (other) {
		//---------------------------------------
		// All other objects should be updated...
		#ifdef LAB_ONLY
		__int64 x;
		x=GetCycles();
		#endif
	
		if (turrets) 
		{
			for (i=0;i<numTurrets;i++) 
			{
				if (turrets[i] && turrets[i]->getExists()) 
				{
			#ifdef LAB_ONLY
				MCTimeAnimationandMatrix =
				MCTimePerShapeTransform =
				MCTimeTransformandLight = 0;
			#endif
					if (!turrets[i]->update())
						turrets[i]->setExists(false);
			#ifdef LAB_ONLY
				MCTimeTurretsTL += MCTimeTransformandLight;
			#endif
				}
			}
		}
		
		#ifdef LAB_ONLY
		x=GetCycles()-x;
		MCTimeTurretsUpdate = x;
		
		x=GetCycles(); 
		#endif
		
		if (weapons) {
			for (i=0;i<numWeapons;i++) {
				if (weapons[i] && weapons[i]->getExists()) {
					if (!weapons[i]->update())
						weapons[i]->setExists(false);
				}
			}
		}

		if (carnage) {
			for (i = 0; i < numCarnage; i++) {
				if (carnage[i] && carnage[i]->getExists()) {
					if (!carnage[i]->update())
						carnage[i]->setExists(false);
				}
			}
		}

		if (lights) {
			for (i = 0; i < numLights; i++) {
				if (lights[i] && lights[i]->getExists()) {
					if (!lights[i]->update())
						lights[i]->setExists(false);
				}
			}
		}

		if (artillery) {
			for (i = 0; i < numArtillery; i++) {
				if (artillery[i] && artillery[i]->getExists()) {
					if (!artillery[i]->update())
						artillery[i]->setExists(false);
				}
			}
		}
		
		#ifdef LAB_ONLY
		x=GetCycles()-x;
		MCTimeAllElseUpdate = x;
		#endif
	}
	if (mc2ObjSplit) MC2_ObjOtherMs += std::chrono::duration<double, std::milli>(MC2ObjClock::now() - mc2ObjT).count();
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::get (GameObjectHandle handle) {

	if ((handle < 1) || (handle > getMaxObjects()))
		return(NULL);

	return(objList[handle]);
}

//---------------------------------------------------------------------------

// PORT (M8-B45): WHAT IS ACTUALLY IN THE MOVER LIST?
//
// Measured: the home team's sensors spot MECHS only -- Werewolf, ShaYu, Anubis,
// Wolfhound -- and never a single VEHICLE, while Legions, Storms, Vedettes and
// Hetzers all read CONTACT_NONE at any range. That is the owner's symptom
// exactly: enemy mechs visible, enemy tanks invisible.
//
// SensorSystem::scanBattlefield walks ObjectManager's moverList, and this is the
// function that fills it. Anything whose getTeam() is NULL is skipped here, and
// skipped SILENTLY -- it would then never be scanned, never become a contact and
// never be drawn, while GroundVehicle::update still runs on it from the vehicles
// array. So the list's composition is the thing to measure, and a run where the
// vehicle count matches numVehicles clears this outright.
//
// File-scope and read once (R1-19).
static const bool MC2_MoverListCensus = []() {
	const char* e = getenv("MC2_GV_FACE_CENSUS");
	return e && *e && strcmp(e, "0") != 0;
}();

long GameObjectManager::buildMoverLists (void) {
	long i;

	numMovers = 0;
	numGoodMovers = 0;
	numBadMovers = 0;

	// PORT (M8-B45): WALK THE WHOLE ARRAY, NOT numMechs/numVehicles.
	//
	// THIS IS A DELIBERATE DIVERGENCE FROM RETAIL, and it is the fix for the
	// owner's invisible units. Measured, on their own save:
	//
	//   [load-counts] Load placed mechs=158 vehicles=180;
	//                 arrays say numMechs=38 numVehicles=29 (max 158/180)
	//
	// GameObjectManager::Load fills mechs[] and vehicles[] across the whole array,
	// but never updates numMechs/numVehicles -- and neither does pristine. This
	// function then walked only the first 38 and 29 entries, so every live unit
	// sitting past those indices was left out of moverList for good.
	//
	// SensorSystem::scanBattlefield walks moverList and nothing else, so a unit
	// missing from it can never become a contact: permanently unspotted, no
	// tac-map blip, not targetable by click -- while it still updates, moves and
	// FIRES. That is exactly what the owner reported, and their log caught it:
	//
	//   [hit] 'Anubis' was hit by 'Storm' class=3 distance=50 m contactStatus=0
	//   [hit]      inMoverList=0 ... team=set disabled=0 removed=0
	//
	// Bounding by max instead of the stale counts costs one pass over slots that
	// are usually NULL or team-less, both of which are skipped below anyway. The
	// counts themselves are deliberately NOT changed here: newMech/newVehicle use
	// them as the next free slot, and rewriting them would change allocation
	// behaviour for a bug that lives entirely in this list.
	for (i = 0; i < maxMechs; i++) {
		if (!mechs[i])
			continue;
		MoverPtr mover = dynamic_cast<MoverPtr>(mechs[i]);
		if (!mover || !mover->getTeam())
			continue;
		moverList[numMovers++] = mover;
		if (mover->getTeam()->isFriendly(Team::home))
			goodMoverList[numGoodMovers++] = mover;
		else if (mover->getTeam()->isEnemy(Team::home))
			badMoverList[numBadMovers++] = mover;
	}

	for (i = 0; i < maxVehicles; i++) {
		if (!vehicles[i])
			continue;
		MoverPtr mover = dynamic_cast<MoverPtr>(vehicles[i]);
		if (!mover || !mover->getTeam())
			continue;
		moverList[numMovers++] = mover;
		if (mover->getTeam()->isFriendly(Team::home))
			goodMoverList[numGoodMovers++] = mover;
		else if (mover->getTeam()->isEnemy(Team::home))
			badMoverList[numBadMovers++] = mover;
	}

	if (MC2_MoverListCensus)
	{
		long mechsIn = 0, vehIn = 0, mechsSkipped = 0, vehSkipped = 0;
		for (i = 0; i < numMechs; i++)
			if (dynamic_cast<MoverPtr>(mechs[i])->getTeam()) ++mechsIn; else ++mechsSkipped;
		for (i = 0; i < numVehicles; i++)
		{
			MoverPtr mv = dynamic_cast<MoverPtr>(vehicles[i]);
			if (mv->getTeam())
				++vehIn;
			else
			{
				++vehSkipped;
				// Name them. If these are the owner's Legions and Storms, the search
				// ends here; if they are wrecks or spent reinforcements, it does not.
				printf("[mover-list]   SKIPPED vehicle '%s' teamId=%ld cmdr=%ld exists=%d destroyed=%d\n",
					mv->getName() ? mv->getName() : "?", (long)mv->getTeamId(),
					(long)mv->getCommanderId(), mv->getExists() ? 1 : 0,
					mv->isDestroyed() ? 1 : 0);
			}
		}
		// calcContactStatus refuses any mover without OBJECT_FLAG_SENSOR, and the
		// measured asymmetry is by CLASS: mechs get spotted, vehicles never do.
		// So count the flag per class. Equal proportions clear it; a vehicle
		// count of zero names the cause outright.
		long mechFlag = 0, vehFlag = 0, mechLive = 0, vehLive = 0;
		for (i = 0; i < numMechs; i++)
		{
			MoverPtr mv = dynamic_cast<MoverPtr>(mechs[i]);
			if (!mv->getExists()) continue;
			++mechLive;
			if (mv->getFlag(OBJECT_FLAG_SENSOR)) ++mechFlag;
		}
		for (i = 0; i < numVehicles; i++)
		{
			MoverPtr mv = dynamic_cast<MoverPtr>(vehicles[i]);
			if (!mv->getExists()) continue;
			++vehLive;
			if (mv->getFlag(OBJECT_FLAG_SENSOR)) ++vehFlag;
		}
		printf("[mover-list] OBJECT_FLAG_SENSOR: mechs %ld of %ld live, vehicles %ld of %ld live\n",
			mechFlag, mechLive, vehFlag, vehLive);
		// GameObject::lineOfSight indexes moverLineOfSightTable at
		//     handle * maxMovers + target->handle
		// on a table of maxMovers * maxMovers bytes. Any handle at or above
		// maxMovers reads and writes outside its row -- and LOS succeeded 164 times
		// for mech targets and ZERO times in 48,755 checks for vehicle targets, which
		// is the shape of an index that is wrong for the later-allocated class.
		// Handles below maxMovers for both classes would clear this outright.
		{
			long mechMin = 999999, mechMax = -1, vehMin = 999999, vehMax = -1;
			for (i = 0; i < numMechs; i++)
			{
				MoverPtr mv = dynamic_cast<MoverPtr>(mechs[i]);
				if (!mv->getExists()) continue;
				const long h = (long)mv->getHandle();
				if (h < mechMin) mechMin = h;
				if (h > mechMax) mechMax = h;
			}
			for (i = 0; i < numVehicles; i++)
			{
				MoverPtr mv = dynamic_cast<MoverPtr>(vehicles[i]);
				if (!mv->getExists()) continue;
				const long h = (long)mv->getHandle();
				if (h < vehMin) vehMin = h;
				if (h > vehMax) vehMax = h;
			}
			printf("[mover-list] handles: maxMovers=%ld  mech %ld..%ld  vehicle %ld..%ld  (table is maxMovers*maxMovers bytes)\n",
				(long)maxMovers, mechMin, mechMax, vehMin, vehMax);
		}
		printf("[mover-list] built: numMovers=%ld  mechs %ld of %ld (skipped %ld)  vehicles %ld of %ld (skipped %ld)\n",
			(long)numMovers, mechsIn, (long)numMechs, mechsSkipped,
			vehIn, (long)numVehicles, vehSkipped);
		fflush(stdout);
	}

	return(NO_ERR);
}

//---------------------------------------------------------------------------

bool GameObjectManager::modifyMoverLists (MoverPtr mover, long action) {
	long i;

	switch (action) {
		case MOVERLIST_DELETE: {
			bool foundIt = false;
			if (mover->getObjectClass() == BATTLEMECH) {
				long i = 0;
				for (i = 0; i < numMechs; i++) {
					if (mechs[i] == mover)
						break;
				}
				if (i < numMechs) {
					foundIt = true;
					BattleMechPtr mech = mechs[i];
					memmove(&mechs[i], &mechs[i + 1], (maxMechs - i - 1) * sizeof(BattleMechPtr));
					mechs[maxMechs - 1] = mech;
					numMechs--;
				}
				}
			else if (mover->getObjectClass() == GROUNDVEHICLE) {
				long i = 0;
				for (i = 0; i < numVehicles; i++) {
					if (vehicles[i] == mover)
						break;
				}
				if (i < numVehicles) {
					foundIt = true;
					GroundVehiclePtr vehicle = vehicles[i];
					memmove(&vehicles[i], &vehicles[i + 1], (maxVehicles - i - 1) * sizeof(GroundVehiclePtr));
					vehicles[maxVehicles - 1] = vehicle;
					numVehicles--;
				}
			}

			for (i = 0; i < numMovers; i++)
				if (moverList[i] == mover) {
					moverList[i] = moverList[--numMovers];
					break;
				}

			if (foundIt && mover->getTeam()) {
				if (mover->getTeam()->isFriendly(Team::home)) {
					long i = 0;
					for (i = 0; i < numGoodMovers; i++)
						if (mover == goodMoverList[i])
							break;
					if (i < numGoodMovers)
						goodMoverList[i] = goodMoverList[--numGoodMovers];
					}
				else if (mover->getTeam()->isEnemy(Team::home)) {
					long i = 0;
					for (i = 0; i < numBadMovers; i++)
						if (mover == badMoverList[i])
							break;
					if (i < numBadMovers)
						badMoverList[i] = badMoverList[--numBadMovers];
				}
			}
			return(foundIt);
		}
		case MOVERLIST_ADD:
			moverList[numMovers++] = mover;
			if (mover->getTeam()) {
				if (mover->getTeam()->isFriendly(Team::home))
					goodMoverList[numGoodMovers++] = mover;
				else if (mover->getTeam()->isEnemy(Team::home))
					badMoverList[numBadMovers++] = mover;
			}
			break;
		case MOVERLIST_TRADE:
			//-----------------------------------------------
			// First, remove it from whatever list it's on...
			long i = 0;
			for (i = 0; i < numGoodMovers; i++)
				if (mover == goodMoverList[i])
					break;
			if (i < numGoodMovers)
				goodMoverList[i] = goodMoverList[--numGoodMovers];
			else {
				long i = 0;
				for (i = 0; i < numBadMovers; i++)
					if (mover == badMoverList[i])
						break;
				if (i < numBadMovers)
					badMoverList[i] = badMoverList[--numBadMovers];
			}
			//----------------------------------
			// Now, add it to the proper list...
			if (mover->getTeam()) {
				if (mover->getTeam()->isFriendly(Team::home))
					goodMoverList[numGoodMovers++] = mover;
				else if (mover->getTeam()->isEnemy(Team::home))
					badMoverList[numBadMovers++] = mover;
			}
	}
	return(true);
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findObject (Stuff::Vector3D position) {

	float closestDistance = 10000.0;
	GameObjectPtr closestObj = NULL;
	long numObjects = getMaxObjects();
	for (long objIndex = 1; objIndex <= numObjects; objIndex++) {
		GameObjectPtr obj = objList[objIndex];
		Assert(obj != NULL, objIndex, " GameObjectManager.findObject: NULL obj ");
		if (obj->getExists() && !obj->inTransport()) {
			float distanceFromObject = obj->distanceFrom(position);
			if (distanceFromObject < closestDistance)
				if (distanceFromObject < obj->getExtentRadius()) {
					closestObj = obj;
					closestDistance = distanceFromObject;
				}
		}
	}

	return(closestObj);
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findObjectByTypeHandle (long typeHandle) {

	//-------------------------------------------------
	// This function was called findObjectId() in MC...

	long numObjects = getMaxObjects();
	for (long objIndex = 1; objIndex <= numObjects; objIndex++) {
		GameObjectPtr obj = objList[objIndex];
		if (obj->getExists() && (obj->getTypeHandle() == typeHandle))
			return(obj);
	}
	return(NULL);
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findByPartId (long partId) {

	if (partId == 0)
		return(NULL);

	long numObjects = getMaxObjects();
	for (long objIndex = 1; objIndex <= numObjects; objIndex++) 
	{
		GameObjectPtr obj = objList[objIndex];
		if (obj && obj->getExists() && (obj->getPartId() == partId))
			return(obj);
	}
	return(NULL);
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findByBlockVertex (long blockNum, long vertex) {

	//----------------------------------------
	// SLOW compared to using object handle...
	long partId = calcPartId(TERRAINOBJECT, blockNum, vertex);
	return(findByPartId(partId));
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findByCellPosition (long row, long col) 
{
	//-------------------------------------------------------------------------------------
	// Must implement for Linkage code.  10/20/99 -fs
	// PLEASE DO NOT CALL EVERY FRAME. THIS ONE WILL BE SLOWER THEN CHRISTMAS!!!!!!!!!!!!
	// Store the pointer if at all possible.  YOU HAVE BEEN WARNED!
	long numObjects = getMaxObjects();
	for (long objIndex = 1; objIndex <= numObjects; objIndex++)
	{
		GameObjectPtr obj = objList[objIndex];
		if (obj->getExists())
		{
			long cellR, cellC;
			objList[objIndex]->getCellPosition(cellR,cellC);

			if ((cellR == row) && (cellC == col))
				return objList[objIndex];
		}
	}

	return(NULL);
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findByUnitInfo (long commander, long group, long mate) {

	//----------------------------------------
	// SLOW compared to using object handle...
	long partId = calcPartId(MOVER, commander, group, mate);
	return(findByPartId(partId));
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findObjectByMouse (long mouseX,
													long mouseY,
													GameObjectPtr* searchList,
													long listSize,
													bool skipDisabled) {

	if (!searchList)
		Fatal(0, " GameObjectManager.findObjectByMouse: NULL searchList ");

	for (long objIndex = 0; objIndex < listSize; objIndex++) 
	{
		if (searchList[objIndex] && searchList[objIndex]->getExists()) 
		{
			GameObjectPtr obj = searchList[objIndex];
			Assert(obj != NULL, objIndex, " GameObjectManager.findObjectByMouse: NULL obj ");
			AppearancePtr objAppearance = obj->getAppearance();
			if (objAppearance && objAppearance->canBeSeen()) 
			{
				if (obj->getWindowsVisible() == (turn - VISIBLE_THRESHOLD))
				{
					//-----------------------------------------------------
					float tlx = objAppearance->upperLeft.x;
					float tly = objAppearance->upperLeft.y;
					float brx = objAppearance->lowerRight.x;
					float bry = objAppearance->lowerRight.y;
						
					if ((mouseX >= tlx) && 
						(mouseX <= brx) &&
						(mouseY >= tly) &&
						(mouseY <= bry)) 
					{
						//---------------------------
						// We're on it, so save it...
						if (!obj->isMover() || (obj->isMover() && obj->isOnGUI() && Terrain::IsGameSelectTerrainPosition(obj->getPosition())))
						{
							if (skipDisabled) 
							{
								if (!obj->isDisabled() && 
									(obj->getObjectClass() != TREE) && 
									(obj->getDamageLevel() != 36000000) && 				//We are a rock clump
									objAppearance->PerPolySelect(mouseX, mouseY))
									return(obj);
							}
							else
							{
								//Do not target trees or artillery strikes!!
								if ((obj->getObjectClass() != TREE) && 
									(obj->getObjectClass() != ARTILLERY) &&                                  									(obj->getDamageLevel() != 36000000) && 				//We are a rock clump
									(obj->getDamageLevel() != 36000000) && 				//We are a rock clump
									objAppearance->PerPolySelect(mouseX, mouseY))
									return(obj);
							}
						}
					}
				}
			}
		}
	}
	
	return(NULL);
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findMoverByMouse (long mouseX,
												   long mouseY,
												   long commanderId,
												   bool skipDisabled) {

	GameObjectPtr* searchList = NULL;
	long numMovers = getMaxMovers();
	if (objList)
		searchList = &objList[1];

	if (!searchList)
		return(NULL);

	if (commanderId == -1)
		for (long objIndex = 0; objIndex < numMovers; objIndex++) 
		{
			if (searchList[objIndex] && searchList[objIndex]->getExists()) 
			{
				GameObjectPtr obj = searchList[objIndex];
				Assert(obj != NULL, objIndex, " GameObjectManager.findObjectByMouse: NULL obj ");
				AppearancePtr objAppearance = obj->getAppearance();
				if (objAppearance && objAppearance->canBeSeen()) 
				{
					if (obj->getWindowsVisible() == (turn - VISIBLE_THRESHOLD)) 
					{
						//-----------------------------------------------------
						float tlx = objAppearance->upperLeft.x;
						float tly = objAppearance->upperLeft.y;
						float brx = objAppearance->lowerRight.x;
						float bry = objAppearance->lowerRight.y;
							
						if ((mouseX >= tlx) && 
							(mouseX <= brx) &&
							(mouseY >= tly) &&
							(mouseY <= bry)) 
						{
							//---------------------------
							// We're on it, so save it...
							// Movers are NOT per poly!!
							if (!obj->isMover() || (obj->isMover() && obj->isOnGUI() && Terrain::IsGameSelectTerrainPosition(obj->getPosition())))
							{
								if (skipDisabled) 
								{
									if (!obj->isDisabled())
										return(obj);
								}
								else
									return(obj);
							}
						}
					}
				}
			}
		}
	else
		for (long objIndex = 0; objIndex < numMovers; objIndex++) 
		{
			if (searchList[objIndex] && searchList[objIndex]->getExists()) 
			{
				GameObjectPtr obj = searchList[objIndex];
				Assert(obj != NULL, objIndex, " GameObjectManager.findObjectByMouse: NULL obj ");
				if (obj->getCommanderId() == commanderId)
					continue;
				AppearancePtr objAppearance = obj->getAppearance();
				if (objAppearance && objAppearance->canBeSeen()) 
				{
					if (obj->getWindowsVisible() == (turn - VISIBLE_THRESHOLD)) 
					{
						//-----------------------------------------------------
						float tlx = objAppearance->upperLeft.x;
						float tly = objAppearance->upperLeft.y;
						float brx = objAppearance->lowerRight.x;
						float bry = objAppearance->lowerRight.y;
							
						if ((mouseX >= tlx) && 
							(mouseX <= brx) &&
							(mouseY >= tly) &&
							(mouseY <= bry)) 
						{
							if (!obj->isMover() || (obj->isMover() && obj->isOnGUI() && Terrain::IsGameSelectTerrainPosition(obj->getPosition())))
							{
								//---------------------------
								// We're on it, so save it...
								if (skipDisabled) 
								{
									if (!obj->isDisabled())
										return(obj);
								}
								else
									return(obj);
							}
						}
					}
				}
			}
		}

	return(NULL);
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findTerrainObjectByMouse (long mouseX,
														   long mouseY,
														   bool skipDisabled) 
{
	for (long terrainBlock = 0; terrainBlock < Terrain::numObjBlocks; terrainBlock++) 
	{
		if (Terrain::objBlockInfo[terrainBlock].active) 
		{
			long numObjs = Terrain::objBlockInfo[terrainBlock].numObjects;
			long objIndex = Terrain::objBlockInfo[terrainBlock].firstHandle;
			GameObjectPtr obj = findObjectByMouse(mouseX, mouseY, &objList[objIndex], numObjs, skipDisabled);
			if (obj)
				return(obj);
		}
	}

	return(NULL);
}

//---------------------------------------------------------------------------

GameObjectPtr GameObjectManager::findObjectByMouse (long mouseX, long mouseY) {

	GameObjectPtr obj = NULL;

	//-------------------------------------------------------------
	// This function will search movers first. If we find an object
	// near the mouse, we're done...
	// Mitch wants to find live ones before dead ones
	long homeCommanderId = Commander::home->getId();
	obj = findMoverByMouse(mouseX, mouseY, homeCommanderId, true);
	if (obj)
		return(obj);
	obj = findMoverByMouse(mouseX, mouseY, homeCommanderId, false);
	if (obj)
		return(obj);
	
	obj = findMoverByMouse(mouseX, mouseY, -1, true);
	if (obj)
		return(obj);
	obj = findMoverByMouse(mouseX, mouseY, -1, false);
	if (obj)
		return(obj);
	

	//-------------------------------------------------------------------------
	// If we still haven't found anything, check everything (including terrain)
	// with disregard to disabled status...
	// Skip disabled should be on.  I have Spoken! -fs
	// Frank -- I need to find the nav markers so I can get their little highlight text.... HKG
	obj = findObjectByMouse(mouseX, mouseY, &objList[1], getMaxObjects(),false);
	if (obj)
		return(obj);
		
	return(findTerrainObjectByMouse(mouseX, mouseY,true));
}

//---------------------------------------------------------------------------
bool GameObjectManager::moverInRect(long index, Stuff::Vector3D &dStart, Stuff::Vector3D &dEnd)
{
	//------------------------------------------------------------------------------
	// This function checks the mover passed in to see if it
	// is within the magic rectangle.  It assumes we are looking for our alignment.
	// This is because we are drag selecting in the GUI!
	if ((index < 0) || (index >= getMaxMovers()))
		return(false);
		
	MoverPtr checkMover = getMover(index);
	if (checkMover && checkMover->getExists() && (checkMover->getTeam() == Team::home))
	{
		AppearancePtr objAppearance = checkMover->getAppearance();
		if (objAppearance /*&& objAppearance->canBeSeen() */) 
		{
//			if (checkMover->getWindowsVisible() > (turn - VISIBLE_THRESHOLD)) 
			{
				//-----------------------------------------------------
				// Note that this effectively tests inTransport, since
				// windowsVisible only gets set when not inTransport...
				//objAppearance->recalcBounds();	//Shouldn't need to do this.  They should already be correct!
				long left 	= fmin(dStart.x, dEnd.x);
				long right 	= fmax(dStart.x, dEnd.x);
				long top 	= fmin(dStart.y, dEnd.y);
				long bottom = fmax(dStart.y, dEnd.y);
				
				if ((left <= objAppearance->getScreenPos().x) && 
					(right >= objAppearance->getScreenPos().x) &&
					(top <= objAppearance->getScreenPos().y) &&
					(bottom >= objAppearance->getScreenPos().y))
				{
					return(true);
				}
			}
		}
	}
	
	return(false);
}	

//---------------------------------------------------------------------------

ObjectTypePtr GameObjectManager::loadObjectType (ObjectTypeNumber typeHandle) {

	return(objTypeManager->load(typeHandle));
}

//---------------------------------------------------------------------------

ObjectTypePtr GameObjectManager::getObjectType (ObjectTypeNumber typeHandle) {

	return(objTypeManager->get(typeHandle));
}

//---------------------------------------------------------------------------

void GameObjectManager::removeObjectType (ObjectTypeNumber typeHandle) {

	objTypeManager->remove(typeHandle);
}

//---------------------------------------------------------------------------

GameObjectHandle GameObjectManager::getHandle (GameObjectPtr obj) {
	long i;

	for (i = 1; i <= getMaxObjects(); i++)
		if (objList[i] == obj)
			return(i);
	return(0);
}

//---------------------------------------------------------------------------

long GameObjectManager::calcPartId (long objectClass, long param1, long param2, long param3) {

	//-------------------------------------------------------------------
	// This should be the only function used to calc partIds for objects.
	// This will allow easy modification of the formulas without making
	// changes everywhere...
	long partId = 0;
	switch (objectClass) {
		case MOVER:
		case BATTLEMECH:
		case GROUNDVEHICLE:
		case ELEMENTAL:
			if (param1 == -1) {
				if (nextReinforcementPartId == (MAX_REINFORCEMENT_PART_ID + 1))
					return(0);
				partId = nextReinforcementPartId++;
				}
			else
				partId = MIN_MOVER_PART_ID +
						 param1 * MAX_MOVERGROUPS * MAX_MOVERGROUP_COUNT_START +
						 param2 * MAX_MOVERGROUP_COUNT_START +
						 param3;
			break;
		case TERRAINOBJECT:
		case TREE:
		case BUILDING:
		case TREEBUILDING:
		case TURRET:
		case GATE:
			partId = MIN_TERRAIN_PART_ID + param1 * MAX_MAP_CELL_WIDTH + param2;
			break;
		case TRAINCAR:
			Fatal(0, " TRAINS NEED PART IDS ");
			break;
	}
	return(partId);
}

//---------------------------------------------------------------------------

void GameObjectManager::setPartId (GameObjectPtr obj, long param1, long param2, long param3) {

	//------------------------------------------------------------------------
	// ALL game objects should have their partIds set through this function.
	// This simply allows easier modification and monitoring of the partIds,
	// should we have any reason to modify or test 'em as missions are loaded.
	long partId = calcPartId(obj->getObjectClass(), param1, param2, param3);
	obj->setPartId(partId);
}

//---------------------------------------------------------------------------

long GameObjectManager::initCollisionSystem (FitIniFile* missionFile) {

	collisionSystem = new CollisionSystem;
	gosASSERT(collisionSystem != NULL);
		
	collisionSystem->init(missionFile);

	return(0);
}

//---------------------------------------------------------------------------

long GameObjectManager::buildCollidableList (void) 
{
	long i;
	rebuildCollidableList = false;
	
	if (collidableList) 
	{
		ObjectTypeManager::objectCache->Free(collidableList);
		collidableList = NULL;
	}

	// First, how many collidables are there?
	numCollidables = numMechs + numVehicles + numElementals + numTurrets + numGates + numCarnage + numArtillery;

	collidableList = (GameObjectPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(GameObjectPtr) * numCollidables);

	long curIndex = 0;
	for (i = 0; i < numMechs; i++)
		collidableList[curIndex++] = mechs[i];

	for (i = 0; i < numVehicles; i++)
		collidableList[curIndex++] = vehicles[i];

	for (i = 0; i < numElementals; i++)
		collidableList[curIndex++] = (GameObjectPtr)elementals[i];

	for (i=0;i<numTurrets;i++)
		collidableList[curIndex++] = turrets[i];

	for (i=0;i<numGates;i++)
		collidableList[curIndex++] = gates[i];

	for (i=0;i<numCarnage;i++)
		collidableList[curIndex++] = carnage[i];
		
	for (i=0;i<numArtillery;i++)
		collidableList[curIndex++] = artillery[i];

	Assert(curIndex == numCollidables, curIndex, " GameObjectManager.buildCollidableList: oof ");
	
	return(0);
}

//---------------------------------------------------------------------------

long GameObjectManager::getCollidableList (GameObjectPtr*& objList) 
{
	if (rebuildCollidableList)
		buildCollidableList();
		
	objList = collidableList;
	return(numCollidables);
}

//---------------------------------------------------------------------------

long GameObjectManager::updateCollisions (void) {

	if (!collidableList)
		buildCollidableList();

	collisionSystem->checkObjects();

	return(0);
}

//---------------------------------------------------------------------------

void GameObjectManager::detectStaticCollision (GameObjectPtr obj1, GameObjectPtr obj2) {

	collisionSystem->detectStaticCollision(obj1, obj2);
}

//---------------------------------------------------------------------------

void GameObjectManager::updateCaptureList (void) {
	long i;

	for (i = 0; i < MAX_TEAMS; i++)
		numCaptures[i] = 0;
	for (i = 0; i < getNumMovers(); i++) {
		MoverPtr mover = getMover(i);
		if (mover->isDisabled())
			continue;
		MechWarriorPtr pilot = mover->getPilot();
		if (pilot) {
			TacticalOrderPtr tacOrder;
			tacOrder = pilot->getCurTacOrder();
			if (tacOrder->code == TACTICAL_ORDER_CAPTURE) {
				captureList[mover->getTeamId()][numCaptures[mover->getTeamId()]++] = tacOrder->targetWID;
			}
		}
	}
}

//---------------------------------------------------------------------------

bool GameObjectManager::isTeamCapturing (TeamPtr team, GameObjectWatchID targetWID) {
	long i;

	if (team) {
		long teamID = team->getId();
		for (i = 0; i < numCaptures[teamID]; i++)
			if (targetWID == captureList[teamID][i])
				return(true);
		}
	else {
		for (long teamID = 0; teamID < MAX_TEAMS; teamID++)
			for (i = 0; i < numCaptures[teamID]; i++)
				if (targetWID == captureList[teamID][i])
					return(true);
	}
	return(false);
}

//---------------------------------------------------------------------------

CarnagePtr GameObjectManager::createFire (ObjectTypeNumber fireObjTypeHandle,
										  GameObjectPtr owner,
										  Stuff::Vector3D& position,
										  float tonnage) {

	CarnagePtr fire = getCarnage(CARNAGE_FIRE);
	if (fire) {
		ObjectTypePtr objectType = getObjectType(fireObjTypeHandle);
		if (objectType) {
			//------------------------------------------------------------
			// Make sure the object type we loaded is really a fire object
			// type. If not, NULL out of here...
			if (objectType->getObjectClass() != FIRE)
				return(NULL);
			fire->init(false, objectType);
			fire->setOwner(owner);
			fire->setTonnage(tonnage);
			fire->setPosition(position);
			fire->setExists(true);
		}
	}
	return(fire);
}

//---------------------------------------------------------------------------

CarnagePtr GameObjectManager::createExplosion (long effectId,
											   GameObjectPtr owner,
											   Stuff::Vector3D& position,
											   float damage,
											   float radius) 
{
	long explosionObjTypeHandle = weaponEffects->GetEffectObjNum(effectId);
	if (explosionObjTypeHandle != INVALID_OBJECT) 
	{
		CarnagePtr explosion = getCarnage(CARNAGE_EXPLOSION);
		if (explosion)
		{
			ObjectTypePtr objectType = getObjectType(explosionObjTypeHandle);
			if (objectType && (objectType->getObjectClass() == EXPLOSION))
			{
				//Call in this order or badness.
				explosion->init(effectId);
				explosion->init(false, objectType);
				explosion->setPosition(position);
				explosion->setExtentRadius(radius);
				explosion->setExplDmg(damage);
				explosion->setExists(true);
				explosion->setOwner(owner);
				return(explosion);
			}
			else if (objectType && (objectType->getObjectClass() == FIRE))
			{
				//Call in this order or badness.
				explosion->init(effectId);
				explosion->init(false, objectType);
				explosion->setPosition(position);
				explosion->setExtentRadius(radius);
				explosion->setExplDmg(damage);
				explosion->setExists(true);
				explosion->setOwner(owner);
				return(explosion);
			}
			else
			{
				//Turn on when effects are officially working!
				// Don't really care if nothing happens.  Should be a bug not a crash!
				// -fs
			}
		}
		else
		{
			//Should we warn if we are out of explosions or just go on?
			//-fs
		}
	}

	return(NULL);
}

//---------------------------------------------------------------------------

LightPtr GameObjectManager::createLight (ObjectTypeNumber lightObjTypeHandle) {

	LightPtr light = getLight();
	if (light) {
		ObjectTypePtr objectType = getObjectType(lightObjTypeHandle);
		light->init(false, objectType);
//		light->setOwner(owner);
//		light->setTonnage(tonnage);
		Stuff::Vector3D startPosition;
		startPosition.Zero();
		light->setPosition(startPosition);
		light->setExists(true);
	}
	return(light);
}

//---------------------------------------------------------------------------
WeaponBoltPtr GameObjectManager::createWeaponBolt (long effectId) 
{
	WeaponBoltPtr weaponBolt = getWeapon();
	if (weaponBolt) 
	{
		ObjectTypeNumber weaponBoltObjTypeHandle = weaponEffects->GetEffectObjNum(effectId);
		ObjectTypePtr objectType = getObjectType(weaponBoltObjTypeHandle);
		if (!objectType)
			STOP(("Object Type for a weapon Bolt was NULL.  EffectId: %d  ObjType: %d",effectId, weaponBoltObjTypeHandle));
		if (objectType->getObjectClass() != WEAPONBOLT)
			return(NULL);
		//ALWAYS CALL IN THIS ORDER OR NO EFFECT!!!!!!!!!!!!!!!!!!!!
		weaponBolt->init(effectId);
		weaponBolt->init(false, objectType);
		weaponBolt->setExists(true);
	}

	return(weaponBolt);
}

//---------------------------------------------------------------------------

ArtilleryPtr GameObjectManager::createArtillery (long artilleryType, Stuff::Vector3D& position) 
{
	ArtilleryPtr artillery = getArtillery();
	if (artillery) 
	{
		static long strikeObjectId[NUM_ARTILLERY_TYPES] = 
		{
			SMALL_ARTLRY, BIG_ARTLRY, SENSOR_ARTLRY
		};
		
 		ObjectTypePtr objectType = getObjectType(strikeObjectId[artilleryType]);
		artillery->init(false, objectType);
		artillery->setPosition(position);
		artillery->setExists(true);
	}
	
	return(artillery);
}

void GameObjectManager::updateAppearancesOnly( bool terrain, bool movers, bool other)
{
	long i;

	if (terrain && renderObjects) 
	{
		for (long terrainBlock = 0; terrainBlock < Terrain::numObjBlocks; terrainBlock++) 
		{
			if (Terrain::objBlockInfo[terrainBlock].active) 
			{
				long numObjs = Terrain::objBlockInfo[terrainBlock].numObjects;
				long objIndex = Terrain::objBlockInfo[terrainBlock].firstHandle;
				for (long terrainObj = 0; terrainObj < numObjs; terrainObj++, objIndex++) 
				{
					if (objList[objIndex] && 
						(Terrain::objVertexActive[objList[objIndex]->getVertexNum()] || (turn < 3)) && 
						objList[objIndex]->getExists())
					{
						if (objList[objIndex]->getAppearance()->recalcBounds()) 
						{
							// Must force here as well.
							Stuff::Vector3D pos = objList[objIndex]->getPosition();
							float rot = objList[objIndex]->getRotation();
							pos.z = land->getTerrainElevation(pos);
							long drawFlags = objList[objIndex]->drawFlags;
							int teamID = objList[objIndex]->getTeamId();
							objList[objIndex]->getAppearance()->setObjectParameters(pos,rot,drawFlags,teamID,Team::getRelation(teamID, Team::home->getId()));

							objList[objIndex]->getAppearance()->update( false );
							objList[objIndex]->getAppearance()->setVisibility(true, true);	
						}
					}
				}
			}
		}
	}

	if (movers) 
	{
		if (mechs) 
		{
			for (i = 0; i < maxMechs; i++)
				if (mechs[i] && mechs[i]->getExists())
				{
					bool inView = mechs[i]->getAppearance()->recalcBounds();
					if (inView)
						mechs[i]->windowsVisible = turn;
					
					// Must force here as well.
					Stuff::Vector3D pos = mechs[i]->getPosition();
					float rot = mechs[i]->getRotation();
					pos.z = land->getTerrainElevation(pos);
					bool selected = mechs[i]->isSelected();
					int teamID = mechs[i]->getTeamId();
					mechs[i]->getAppearance()->setObjectParameters(pos,rot,mechs[i]->drawFlags,teamID,Team::getRelation(teamID, Team::home->getId()));

					mechs[i]->getAppearance()->update( false );
					mechs[i]->getAppearance()->setVisibility(inView,true);
				}
		}

		if (vehicles) 
		{
			for (i = 0; i < maxVehicles; i++)
				if (vehicles[i] && vehicles[i]->getExists())
				{
					bool inView = vehicles[i]->getAppearance()->recalcBounds();
					vehicles[i]->windowsVisible = turn;

					// Must force here as well.
					Stuff::Vector3D pos = vehicles[i]->getPosition();
					float rot = vehicles[i]->getRotation();
					pos.z = land->getTerrainElevation(pos);
					if ((pos.z < MapData::waterDepth))
						pos.z = MapData::waterDepth;
					bool selected = vehicles[i]->isSelected();
					int teamID = vehicles[i]->getTeamId();
					vehicles[i]->getAppearance()->setObjectParameters(pos, rot, vehicles[i]->drawFlags, teamID, Team::getRelation(teamID, Team::home->getId()));

					// PORT (M8-B47): CARRY THE TURRET ROTATION THROUGH A PAUSE.
					//
					// GroundVehicle::update sets BOTH the object parameters and
					// setMoverParameters(turretRotation, ...) every frame. This paused
					// path set only the object parameters and then called update(),
					// so the shape was rebuilt with whatever turret rotation the
					// appearance happened to be holding -- and the owner sees the tops
					// of the tanks swing round on pause and back again on unpause.
					//
					// It only became visible once M8-B46 made these vehicles update
					// while paused at all; before that they simply vanished, which hid
					// it. Pristine omits this call too, so this is a DELIBERATE
					// DIVERGENCE -- and a small one: the same call the live path makes,
					// with the zero body deltas it itself uses when the hull is still.
					vehicles[i]->getAppearance()->setMoverParameters(vehicles[i]->turretRotation,
					                                                0.0f, 0.0f,
					                                                !vehicles[i]->pathLocks);

					vehicles[i]->getAppearance()->update( false );
					vehicles[i]->getAppearance()->setInView( inView );
				}
		}

	}

	if (other) {
		//---------------------------------------
		// All other objects should be updated...
		if (turrets) 
		{
			for (i=0;i<numTurrets;i++) 
			{
				if (turrets[i] && turrets[i]->getExists()) 
				{
					if (turrets[i]->getAppearance()->recalcBounds())
					{
						// Must force here as well.
						Stuff::Vector3D pos = turrets[i]->getPosition();
						float rot = turrets[i]->getRotation();
						pos.z = land->getTerrainElevation(pos);
						bool selected = turrets[i]->isSelected();
						int teamID = turrets[i]->getTeamId();
						turrets[i]->getAppearance()->setObjectParameters(pos,rot,selected,teamID,Team::getRelation(teamID, Team::home->getId()));

						turrets[i]->getAppearance()->update( false );
						turrets[i]->getAppearance()->setInView( 1 );
					}
					else
						turrets[i]->getAppearance()->setInView( 0 );
				}
			}
		}
		
/*		if (weapons) {
			for (long i=0;i<numWeapons;i++) {
				if (weapons[i] && weapons[i]->getExists()) {
					if (!weapons[i]->update())
						weapons[i]->setExists(false);
				}
			}
		}*/

		/*if (carnage) {
			for (long i = 0; i < numCarnage; i++) {
				if (carnage[i] && carnage[i]->getExists()) {
					if (!carnage[i]->update())
						carnage[i]->setExists(false);
				}
			}
		}*/

		if (lights) 
		{
			for (i = 0; i < numLights; i++) 
			{
				if (lights[i] && lights[i]->getExists()) 
				{
					if (lights[i]->getAppearance()->recalcBounds() )
					{
						lights[i]->getAppearance()->update( false );
						lights[i]->getAppearance()->setInView( 1 );
					}
					else
						lights[i]->getAppearance()->setInView(0);
				}
			}
		}

		if (artillery) 
		{
			for (i = 0; i < numArtillery; i++) 
			{
				if (artillery[i] && artillery[i]->getExists()) 
				{
					if (artillery[i]->getAppearance()->recalcBounds() )
					{
						artillery[i]->getAppearance()->update( false );
						artillery[i]->getAppearance()->setInView( 1 );
					}
					else
						artillery[i]->getAppearance()->setInView( 0 );
				}
			}
		}
	}
}

//-------------------------------------------------------------------
void GameObjectManager::CopyTo (ObjectManagerData *data)
{
	data->maxObjects 		= getMaxObjects();
	data->numElementals		= numElementals;
	data->numTerrainObjects = numTerrainObjects;
	data->numBuildings		= numBuildings;
	data->numTurrets		= numTurrets;
	data->numWeapons		= numWeapons;
	data->numCarnage		= numCarnage;
	data->numLights			= numLights;
	data->numArtillery		= numArtillery;
	data->numGates			= numGates;
	data->maxMechs			= maxMechs;
	data->maxVehicles		= maxVehicles;
	data->numMechs			= numMechs;
	data->numVehicles		= numVehicles;
	data->nextWatchId		= nextWatchID;
}

//-------------------------------------------------------------------
void GameObjectManager::CopyFrom (ObjectManagerData *data)
{
	numElementals		=    data->numElementals;
	numTerrainObjects   =    data->numTerrainObjects;
	numBuildings        =    data->numBuildings;
	numTurrets          =    data->numTurrets;
	numWeapons          =    data->numWeapons;
	numCarnage          =    data->numCarnage;
	numLights           =    data->numLights;
	numArtillery        =    data->numArtillery;
	numGates            =    data->numGates;
	maxMechs            =    data->maxMechs;
	maxVehicles         =    data->maxVehicles;
	numMechs            =    data->numMechs;
	numVehicles         =    data->numVehicles;
	nextWatchID			=	 data->nextWatchId;
}

//-------------------------------------------------------------------
long GameObjectManager::Save (PacketFilePtr file, long packetNum)
{
	long i;
	long *watchSave = (long *)malloc(sizeof(long) * (getMaxObjects() + 1));
	memset(watchSave,0,sizeof(long) * (getMaxObjects() + 1));

	ObjectManagerData data;
	CopyTo(&data);

	file->writePacket(packetNum,(MemoryPtr)&data,sizeof(ObjectManagerData),STORAGE_TYPE_RAW);
	packetNum++;

	for (i=0;i<=getMaxObjects();i++)
	{
		if (objList[i])
		{
			objList[i]->Save(file,packetNum);
			packetNum++;

			//Find the watchID from the watchlist for this object.
			// If none, let it be.  It'll be zero already
			// DO NOT CALL getWatchID!!!!!!!
			// That will assign them to objects which don't have them!!!!
			for (long j=0;j<=nextWatchID;j++)
			{
				if (watchList[j] == objList[i])
				{
					watchSave[j] = i;
				}
			}
		}
		else
		{
			packetNum++;
		}
	}

	file->writePacket(packetNum,(MemoryPtr)watchSave,sizeof(long) * (getMaxObjects() + 1),STORAGE_TYPE_ZLIB);
	packetNum++;

	free(watchSave);
	watchSave = NULL;

	return packetNum;
}

//-------------------------------------------------------------------
long GameObjectManager::Load (PacketFilePtr file, long packetNum)
{
	ObjectManagerData data;
	file->readPacket(packetNum,(MemoryPtr)&data);
	packetNum++;

	CopyFrom(&data);

	//------------------------------------------------------------------
	//Create the ObjList and WatchList EXACTLY like we normally do.
	GameObject::setInitialize(true);

	//-----------------------------------------------------------
	// First element in list is NULL (handle of 0 is always NULL)
	objList = (GameObjectPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(GameObjectPtr) * (getMaxObjects() + 1));
	memset(objList,0,sizeof(GameObjectPtr) * (getMaxObjects() + 1));

	watchList = (GameObjectPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(GameObjectPtr) * (getMaxObjects() + 1));
	memset(watchList,0,sizeof(GameObjectPtr) * (getMaxObjects() + 1));

	long i = 0;
	long curHandle = 1;
	maxMovers = maxMechs + maxVehicles + numElementals;
	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (maxMechs > 0) 
	{
		mechs = (BattleMechPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(BattleMechPtr) * maxMechs);
		if (!mechs)
			Fatal(maxMechs, " GameObjectManager.setNumObjects: cannot malloc mechs ");
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (maxVehicles > 0) 
	{
		vehicles = (GroundVehiclePtr*)ObjectTypeManager::objectCache->Malloc(sizeof(GroundVehiclePtr) * maxVehicles);
		if (!vehicles)
			Fatal(maxVehicles, " GameObjectManager.setNumObjects: cannot malloc vehicles ");
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (numTerrainObjects > 0) 
	{
		terrainObjects = (TerrainObjectPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(TerrainObjectPtr) * numTerrainObjects);
		if (!terrainObjects)
			Fatal(numTerrainObjects, " GameObjectManager.setNumObjects: cannot malloc terrain objects ");
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (numBuildings > 0) 
	{
		buildings = (BuildingPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(BuildingPtr) * numBuildings);
		if (!buildings)
			Fatal(numBuildings, " GameObjectManager.setNumObjects: cannot malloc buildings ");
	}

	if (numTurrets > 0)
	{
		turrets = (TurretPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(TurretPtr) * numTurrets);
		if ( !turrets )
			Fatal(numTurrets, " GameObjectManager.setNumObjects: cannot malloc Turrets ");
	}

	if (numGates > 0)
	{
		gates = (GatePtr*)ObjectTypeManager::objectCache->Malloc(sizeof(GatePtr) * numGates);
		if ( !gates )
			Fatal(numGates, " GameObjectManager.setNumObjects: cannot malloc Gates ");
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (numWeapons > 0) 
	{
		weapons = (WeaponBoltPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(WeaponBoltPtr) * numWeapons);
		if (!weapons)
			Fatal(numWeapons, " GameObjectManager.setNumObjects: cannot malloc weapons ");
	}

	if (numCarnage > 0) 
	{
		carnage = (CarnagePtr*)ObjectTypeManager::objectCache->Malloc(sizeof(CarnagePtr) * numCarnage);
		if (!carnage)
			Fatal(numCarnage, " GameObjectManager.setNumObjects: cannot malloc carnage ");
	}

	if (numArtillery > 0) 
	{
		artillery = (ArtilleryPtr*)ObjectTypeManager::objectCache->Malloc(sizeof(ArtilleryPtr) * numArtillery);
		if (!artillery)
			Fatal(numArtillery, " GameObjectManager.setNumObjects: cannot malloc artillery ");
	}

	useMoverLineOfSightTable = true;
	moverLineOfSightTable = (char*)systemHeap->Malloc(maxMovers * maxMovers);
	if (!moverLineOfSightTable)
		Fatal(numGates, " GameObjectManager.setNumObjects: cannot malloc moverLineOfSightTable ");

	long curTerrObjNum = 0;
	long curBuildingNum = 0;
	long curTurretNum = 0;
	long curGateNum = 0;
	long curArtilleryNum = 0;
	long curCarnageNum = 0;
	long curMechNum = 0;
	long curVehicleNum = 0;
	long curBoltNum = 0;

	long oldBlockNum = -1;
	for (i=0;i<=getMaxObjects();i++)
	{
		//OK.
		// For each object that was saved, Load the data.
		// If objectTypeNum != 0, create the object from the type EXACTLY like we do above.
		// then init from the savedData.
		// Next Object.

		//First, get the size of the packet saved.
		// This will tell me what kind of object it was!
		file->seekPacket(packetNum);
		DWORD packetSize = file->getPacketSize();
		if (packetSize == 0)
		{
			//NO Object stored here.
			// Increment Packet and move on
			// First object in ObjList, for example!!
			packetNum++;
		}
		else if ((packetSize == sizeof(GameObjectData)) ||
				(packetSize == sizeof(MoverData)))
		{
			STOP(("We saved a pure GameObject or Pure Mover in slot %d, packet %d",i,packetNum));
		}
		else if (packetSize == sizeof(TerrainObjectData))
		{
			//We have a TerrainObject.
			// Get its data.
			TerrainObjectData data;
			file->readPacket(packetNum,(MemoryPtr)(&data));
			packetNum++;

			TerrainObjectPtr obj = new TerrainObject; 
			objList[i] = terrainObjects[curTerrObjNum] = obj;
			objList[i]->setHandle(i);

			if (data.objectTypeNum > 0)	//We have an object type to copy from!!
			{
				ObjectTypePtr objType = getObjectType(data.objectTypeNum);
				if (!objType)
					STOP(("We saved an Object we don't know how to re-create %d",data.objectTypeNum));

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
				obj->setDamage(data.damage);
				obj->setRotation(data.rotation);

				if (data.blockNumber != oldBlockNum)
				{
					oldBlockNum = data.blockNumber;
					Terrain::objBlockInfo[data.blockNumber].firstHandle = i;
				}

				Terrain::objBlockInfo[data.blockNumber].numCollidableObjects++;
				Terrain::objBlockInfo[data.blockNumber].numObjects++;
			}
			else
			{
				obj->setExists(false);
			}

			curTerrObjNum++;
		}
		else if (packetSize == sizeof(BuildingData))
		{
			//We have a Building.
			BuildingData data;
			file->readPacket(packetNum,(MemoryPtr)(&data));
			packetNum++;

			BuildingPtr obj = new Building; 
			objList[i] = buildings[curBuildingNum] = obj;
			objList[i]->setHandle(i);

			if (data.objectTypeNum > 0)	//We have an object type to copy from!!
			{
				ObjectTypePtr objType = getObjectType(data.objectTypeNum);
				if (!objType)
					STOP(("We saved an Object we don't know how to re-create %d",data.objectTypeNum));

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
				obj->setDamage(data.damage);
				obj->setRotation(data.rotation);

				if (data.blockNumber != oldBlockNum)
				{
					oldBlockNum = data.blockNumber;
					Terrain::objBlockInfo[data.blockNumber].firstHandle = i;
				}

				if (((((BuildingTypePtr)objType)->perimeterAlarmRange > 0.0f) &&
					(((BuildingTypePtr)objType)->perimeterAlarmTimer > 0.0f)) ||
					(((BuildingTypePtr)objType)->lookoutTowerRange > 0.0f) ||
					(((BuildingTypePtr)objType)->sensorRange > 0.0f))
				{
					Terrain::objBlockInfo[data.blockNumber].numCollidableObjects++;
				}

				Terrain::objBlockInfo[data.blockNumber].numObjects++;
			}
			else
			{
				obj->setExists(false);
			}
			curBuildingNum++;
		}
		else if (packetSize == sizeof(TurretData))
		{
			//We have a Turret.
			TurretData data;
			file->readPacket(packetNum,(MemoryPtr)(&data));
			packetNum++;

			TurretPtr obj = new Turret; 
			objList[i] = turrets[curTurretNum] = obj;
			objList[i]->setHandle(i);

			if (data.objectTypeNum > 0)	//We have an object type to copy from!!
			{
				ObjectTypePtr objType = getObjectType(data.objectTypeNum);
				if (!objType)
					STOP(("We saved an Object we don't know how to re-create %d",data.objectTypeNum));

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
				obj->setDamage(data.damage);
				obj->setRotation(data.rotation);

				if (data.blockNumber != oldBlockNum)
				{
					oldBlockNum = data.blockNumber;
					Terrain::objBlockInfo[data.blockNumber].firstHandle = i;
				}

				Terrain::objBlockInfo[data.blockNumber].numCollidableObjects++;
				Terrain::objBlockInfo[data.blockNumber].numObjects++;
			}
			else
			{
				obj->setExists(false);
			}
			curTurretNum++;
		}
		else if (packetSize == sizeof(GateData))
		{
			//We have a Gate.
			GateData data;
			file->readPacket(packetNum,(MemoryPtr)(&data));
			packetNum++;

			GatePtr obj = new Gate; 
			objList[i] = gates[curGateNum] = obj;
			objList[i]->setHandle(i);

			if (data.objectTypeNum > 0)	//We have an object type to copy from!!
			{
				ObjectTypePtr objType = getObjectType(data.objectTypeNum);
				if (!objType)
					STOP(("We saved an Object we don't know how to re-create %d",data.objectTypeNum));

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
				obj->setDamage(data.damage);
				obj->setRotation(data.rotation);

				if (data.blockNumber != oldBlockNum)
				{
					oldBlockNum = data.blockNumber;
					Terrain::objBlockInfo[data.blockNumber].firstHandle = i;
				}

				Terrain::objBlockInfo[data.blockNumber].numCollidableObjects++;
				Terrain::objBlockInfo[data.blockNumber].numObjects++;
			}
			else
			{
				obj->setExists(false);
			}
			curGateNum++;
		}
		else if (packetSize == sizeof(ArtilleryData))
		{
			//We have an Artillery.
			ArtilleryData data;
			file->readPacket(packetNum,(MemoryPtr)(&data));
			packetNum++;

			ArtilleryPtr obj = new Artillery; 
			objList[i] = artillery[curArtilleryNum] = obj;
			objList[i]->setHandle(i);

			if (data.objectTypeNum > 0)	//We have an object type to copy from!!
			{
				ObjectTypePtr objType = getObjectType(data.objectTypeNum);
				if (!objType)
					STOP(("We saved an Object we don't know how to re-create %d",data.objectTypeNum));

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
			}
			else
			{
				obj->setExists(false);
			}
			curArtilleryNum++;
		}
		else if (packetSize == sizeof(CarnageData))
		{
			//We have a Carnage.
			CarnageData data;
			file->readPacket(packetNum,(MemoryPtr)(&data));
			packetNum++;

			CarnagePtr obj = new Carnage; 
			objList[i] = carnage[curCarnageNum] = obj;
			objList[i]->setHandle(i);

			if (data.objectTypeNum > 0)	//We have an object type to copy from!!
			{
				ObjectTypePtr objType = getObjectType(data.objectTypeNum);
				if (!objType)
					STOP(("We saved an Object we don't know how to re-create %d",data.objectTypeNum));

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
			}
			else
			{
				obj->setExists(false);
			}
			curCarnageNum++;
		}
		else if (packetSize == sizeof(MechData))
		{
			//We have a BattleMech.
			MechData data;
			file->readPacket(packetNum,(MemoryPtr)(&data));
			packetNum++;

			BattleMechPtr obj = new BattleMech; 
			objList[i] = mechs[curMechNum] = obj;
			objList[i]->setHandle(i);

			if (data.objectTypeNum > 0)	//We have an object type to copy from!!
			{
				ObjectTypePtr objType = getObjectType(data.objectTypeNum);
				if (!objType)
					STOP(("We saved an Object we don't know how to re-create %d",data.objectTypeNum));

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
			}
			else
			{
				obj->setExists(false);
			}
			curMechNum++;
		}
		else if (packetSize == sizeof(GroundVehicleData))
		{
			//We have a groundVehicle.
			GroundVehicleData data;
			file->readPacket(packetNum,(MemoryPtr)(&data));
			packetNum++;

			GroundVehiclePtr obj = new GroundVehicle; 
			objList[i] = vehicles[curVehicleNum] = obj;
			objList[i]->setHandle(i);

			if (data.objectTypeNum > 0)	//We have an object type to copy from!!
			{
				ObjectTypePtr objType = getObjectType(data.objectTypeNum);
				if (!objType)
					STOP(("We saved an Object we don't know how to re-create %d",data.objectTypeNum));

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
			}
			else
			{
				obj->setExists(false);
			}
			curVehicleNum++;
		}
		else if (packetSize == sizeof(WeaponBoltData))
		{
			//We have a WeaponBolt.
			WeaponBoltData data;
			file->readPacket(packetNum,(MemoryPtr)(&data));
			packetNum++;

			WeaponBoltPtr obj = new WeaponBolt; 
			objList[i] = weapons[curBoltNum] = obj;
			objList[i]->setHandle(i);

			if (data.objectTypeNum > 0)	//We have an object type to copy from!!
			{
				ObjectTypePtr objType = getObjectType(data.objectTypeNum);
				if (!objType)
					STOP(("We saved an Object we don't know how to re-create %d",data.objectTypeNum));

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
			}
			else
			{
				obj->setExists(false);
			}
			curBoltNum++;
		}
	}

	if (curTerrObjNum != numTerrainObjects)
		STOP(("Didn't load %d but instead %d TerrainObjects",numTerrainObjects,curTerrObjNum));

	if (curBuildingNum != numBuildings)
		STOP(("Didn't load %d but instead %d Buildings",numBuildings,curBuildingNum));

	if (curTurretNum != numTurrets)
		STOP(("Didn't load %d but instead %d Turrets",numTurrets,curTurretNum));

	if (curGateNum != numGates)
		STOP(("Didn't load %d but instead %d Gates",numGates,curGateNum));

	if (curArtilleryNum != numArtillery)
		STOP(("Didn't load %d but instead %d Artillery",numArtillery,curArtilleryNum));

	if (curCarnageNum != numCarnage)
		STOP(("Didn't load %d but instead %d Carnage",numCarnage,curCarnageNum));

	if (curMechNum != maxMechs)
		STOP(("Didn't load %d but instead %d Mechs",maxMechs,curMechNum));

	if (curVehicleNum != maxVehicles)
		STOP(("Didn't load %d but instead %d Vehicles",maxVehicles,curVehicleNum));

	if (curBoltNum != numWeapons)
		STOP(("Didn't load %d but instead %d WeaponBolts",numWeapons,curBoltNum));

	rebuildCollidableList = true;

	//---------------------------------------------------
	// Finally, let's build the control building lists...
	for (i = 0; i < numTurrets; i++) 
	{
		if ((turrets[i]->parentId != 0xffffffff) && (turrets[i]->parentId != 0)) 
		{
			BuildingPtr controlBuilding = (BuildingPtr)ObjectManager->findByCellPosition((turrets[i]->parentId>>16),(turrets[i]->parentId & 0x0000ffff));
			if (controlBuilding && !controlBuilding->getFlag(OBJECT_FLAG_CONTROLBUILDING)) 
			{
				controlBuilding->setFlag(OBJECT_FLAG_CAPTURABLE, true);
				controlBuilding->setFlag(OBJECT_FLAG_CONTROLBUILDING, true);
				controlBuilding->listID = numTurretControls;
				turretControls[numTurretControls++] = controlBuilding;
			}
			else if (!controlBuilding)
			{
				Stuff::Vector3D worldPos;
				if (land)
					land->cellToWorld((turrets[i]->parentId>>16),(turrets[i]->parentId & 0x0000ffff),worldPos);
				PAUSE(("Turret linked to bldg @ R %d, C %d  X:%f Y:%f No Bldg there!",(turrets[i]->parentId>>16),(turrets[i]->parentId & 0x0000ffff),worldPos.x,worldPos.y));
				turrets[i]->parentId = 0xffffffff;
			}
		}
	}

	for (i = 0; i < numGates; i++) 
	{
		if ((gates[i]->parentId != 0xffffffff) && (gates[i]->parentId != 0))
		{
			BuildingPtr controlBuilding = (BuildingPtr)ObjectManager->findByCellPosition((gates[i]->parentId>>16),(gates[i]->parentId & 0x0000ffff));
			if (controlBuilding && !controlBuilding->getFlag(OBJECT_FLAG_CONTROLBUILDING)) 
			{
				controlBuilding->setFlag(OBJECT_FLAG_CONTROLBUILDING, true);
				controlBuilding->listID = numGateControls;
				gateControls[numGateControls++] = controlBuilding;
			}
	   		else if (!controlBuilding)
	   		{
	   			PAUSE(("Gate tried to link to building at Row %d, Col %d.  No Bldg there!",(gates[i]->parentId>>16),(gates[i]->parentId & 0x0000ffff)));
				gates[i]->parentId = 0xffffffff;
	   		}
		}
	}

	//-----------------------------------------------------------------------------------
	// Create list of special buildings.  These buildings will be updated at least once
	// every frame regardless of where they are on the terrain and what is visible.
	// This is because perimeter alarms and lookout buildings and ????? must work even
	// if the player is NOT looking at them!!
	for (i=0;i<numBuildings;i++)
	{
		if (buildings[i]->isSpecialBuilding())
		{
			specialBuildings[numSpecialBuildings++] = buildings[i];
		}
	}
	
 	//--------------------------------------------------------------------
	//Now, lets point every lit building to at least one power generator.
	for (i=0;i<numBuildings;i++)
	{
		if (buildings[i]->isPowerSource())
		{
			powerGenerators[numPowerGenerators++] = buildings[i];
		}
	}
	
	if (numPowerGenerators)
	{
		for (i=0;i<numBuildings;i++)
		{
			Stuff::Vector3D maxDist;
			long generatorIndex = 0;
			maxDist.Subtract(buildings[i]->getPosition(),powerGenerators[0]->getPosition());
			float minDistance = maxDist.GetApproximateLength();
			
			for (long j=1;j<numPowerGenerators;j++)
			{
				maxDist.Subtract(buildings[i]->getPosition(),powerGenerators[j]->getPosition()); 
				float newDistance = maxDist.GetApproximateLength(); 
				
				if (newDistance < minDistance)
				{
					generatorIndex = j;
					minDistance = newDistance;
				}
			}	
			
			buildings[i]->setPowerSupply(powerGenerators[generatorIndex]);
		}
		
		for (i=0;i<numTerrainObjects;i++)
		{
			Stuff::Vector3D maxDist;
			long generatorIndex = 0;
			maxDist.Subtract(terrainObjects[i]->getPosition(),powerGenerators[0]->getPosition());
			float minDistance = maxDist.GetApproximateLength();
			
			for (long j=1;j<numPowerGenerators;j++)
			{
				maxDist.Subtract(terrainObjects[i]->getPosition(),powerGenerators[j]->getPosition()); 
				float newDistance = maxDist.GetApproximateLength(); 
				
				if (newDistance < minDistance)
				{
					generatorIndex = j;
					minDistance = newDistance;
				}
			}	
			
			terrainObjects[i]->setPowerSupply(powerGenerators[generatorIndex]);
		}
	}

	GameObject::setInitialize(false);

	//Reload the Object Watchers
	long *watchSave = (long *)malloc(sizeof(long) * (getMaxObjects() + 1));
	memset(watchSave,0,sizeof(long) * (getMaxObjects() + 1));

	file->readPacket(packetNum,(MemoryPtr)watchSave);
	packetNum++;

	//Find the watchID from the watchlist for this object.
	// If none, let it be.  It'll be zero already
	// DO NOT CALL getWatchID!!!!!!!
	// That will assign them to objects which don't have them!!!!
	// PORT (M8-B20): EVERY UNUSED WATCH SLOT USED TO ALIAS objList[0].
	//
	// watchSave is memset to zero and the save loop only fills the entries that
	// actually had a live object, so "0" means BOTH "no watch here" and "object
	// index 0" -- the format cannot tell them apart. Restoring it unconditionally
	// therefore pointed every unused watch ID at whatever object happens to sit
	// in slot 0. A stale targetWID -- a weapon in flight at save time, an order
	// referencing a unit that has since died -- then resolved to a REAL, LIVE,
	// WRONG object instead of to NULL, and getByWatchID has no way to notice
	// because it only range-checks the id.
	//
	// The objects carry their own watchID through save and load (gameobj.cpp),
	// so the restore can be made self-checking without touching the file format:
	// an entry is only accepted when the object agrees that the slot is its own.
	// Anything else becomes NULL, which is what every caller already handles.
	//
	// NOT CLAIMED: that this is the 2026-08-25 crash. That fault read an
	// unaligned address, which means a garbage `this`, whereas this defect hands
	// back a valid pointer to the wrong object. This is a real bug found while
	// reading the path that crash came through, and it is fixed on its own
	// merits. See the M8-B20 register entry.
	long aliased = 0;
	for (long j=0;j<=getMaxObjects();j++)
	{
		GameObjectPtr candidate = (j < getMaxObjects()) ? objList[watchSave[j]] : NULL;
		if (candidate && candidate->getWatchID() == (unsigned long)j)
			watchList[j] = candidate;
		else if (candidate)
		{
			// MEASURED: clearing these reproducibly crashes the owner's
			// mission-9 save at tacordr.cpp:1289 within a few hundred ticks,
			// because MechWarrior::getVehicle hands the NULL to a caller that
			// does not check. Counted and reported either way; only cleared
			// under MC2_WATCHID_STRICT. See the long note in objmgr.h.
			aliased++;
			watchList[j] = watchIDStrict() ? NULL : candidate;
		}
		else
			watchList[j] = NULL;
	}
	if (aliased)
	{
		printf("[watchlist] load: %ld watch slots do not match their object "
		       "(%s). Set MC2_WATCHID_STRICT=1 to clear them -- read the note in "
		       "objmgr.h first, it crashes the owner's mission-9 save.\n",
		       aliased, watchIDStrict() ? "CLEARED" : "left aliased, as before");
		fflush(stdout);
	}

	free(watchSave);
	watchSave = NULL;

	// PORT (M8-B45): WHAT DID THE LOAD ACTUALLY PLACE, AND WHAT WILL BE REBUILT?
	//
	// Reproduced headless: after loading a save, live units are in NO mover list --
	// among them an undamaged enemy Storm, which is the unit that shot the owner
	// from 50 m reading CONTACT_NONE. Nothing scans a mover that is not in that
	// list, so it can never become a contact: invisible, no tac-map blip, not
	// targetable, and still shooting.
	//
	// buildMoverLists() -- called immediately after this function -- walks
	// mechs[0..numMechs) and vehicles[0..numVehicles). This Load writes into those
	// arrays using its OWN counters and never updates numMechs/numVehicles (nor
	// does pristine). If it placed more than the counts cover, the surplus can
	// never enter the mover list. These two numbers settle that outright: equal
	// pairs clear it, a placed count above the array count is the defect.
	if (MC2_MoverListCensus)
	{
		printf("[load-counts] Load placed mechs=%ld vehicles=%ld; arrays say numMechs=%ld numVehicles=%ld (max %ld/%ld)\n",
			curMechNum, curVehicleNum, (long)numMechs, (long)numVehicles,
			(long)maxMechs, (long)maxVehicles);
		fflush(stdout);
	}

	return packetNum;
}

//***************************************************************************
