//***************************************************************************
//
//	contact.cpp - This file contains the Contact and Sensor Classes code
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef MCLIB_H
#include "mclib.h"
#endif

#ifndef WARRIOR_H
#include "warrior.h"
#endif

#ifndef CONTACT_H
#include "contact.h"
#endif

#ifndef MOVER_H
#include "mover.h"
#endif

//#ifndef GVEHICL_H
//#include "gvehicl.h"
//#endif

#ifndef TEAM_H
#include "Team.h" // PORT: match case-sensitive host path.
#endif

#ifndef UNITDESG_H
#include "unitdesg.h"
#endif

#ifndef SOUNDSYS_H
#include "Soundsys.h" // PORT: match case-sensitive host path.
#endif

#include <GameOS.HPP> // PORT: match case-sensitive host path.

//***************************************************************************

SensorSystemManagerPtr		SensorManager = NULL;

long						SensorSystem::numSensors = 0;
SortListPtr					SensorSystem::sortList = NULL;
float						SensorSystem::scanFrequency = 0.5;

bool						TeamSensorSystem::homeTeamInContact = false;
bool						SensorSystemManager::enemyInLOS = true;

extern float scenarioTime;
extern UserHeapPtr missionHeap;

#define	VISUAL_CONTACT_FLAG	0x8000

//***************************************************************************
// CONTACT INFO class
//***************************************************************************

void* ContactInfo::operator new (size_t ourSize) {

	void* result;
	result = missionHeap->Malloc(ourSize);
	return(result);
}

//---------------------------------------------------------------------------

void ContactInfo::operator delete (void* us) {

	missionHeap->Free(us);
}	

//***************************************************************************
// SENSOR SYSTEM routines
//***************************************************************************

void* SensorSystem::operator new (size_t mySize) {

	void *result = missionHeap->Malloc(mySize);
	return(result);
}

//---------------------------------------------------------------------------

void SensorSystem::operator delete (void* us) {

	missionHeap->Free(us);
}

//---------------------------------------------------------------------------

void SensorSystem::init (void) {

	//id = 0			// this should be set by the sensor system manager only!
	master = NULL;
	masterIndex = -1;
	owner = NULL;
	range = -1.0;
	skill = -1;
	broken = false;
	notShutdown = true;

	ecmEffect = 1.0;

	//ALWAYS true UNLESS this is a sensor probe or a sensor tower!!
	hasLOSCapability = true;
	
	//----------------------------------------------------
	// Don't update any sensors on the very first frame...
	nextScanUpdate = 0.5;
	
	lastScanUpdate = 0.0;
	
	numContacts = 0;
	id = numSensors++;

	numExclusives = 0;
	totalContacts = 0;

	if (!sortList) {
		sortList = new SortList;
		if (!sortList)
			Fatal(0, " Unable to create Contact::sortList ");
		sortList->init(MAX_CONTACTS_PER_SENSOR);
	}
}

//---------------------------------------------------------------------------

void SensorSystem::destroy (void) {

	numSensors--;
	if (numSensors == 0) {
		delete sortList;
		sortList = NULL;
	}
}

//---------------------------------------------------------------------------

void SensorSystem::setMaster (TeamSensorSystemPtr newMaster) {

	master = newMaster;
	broken = false;
}

//---------------------------------------------------------------------------

void SensorSystem::setOwner (GameObjectPtr newOwner) {

	owner = newOwner;
}

//---------------------------------------------------------------------------

void SensorSystem::disable (void) {

	clearContacts();
}

//---------------------------------------------------------------------------

void SensorSystem::setShutdown (bool setting) {
	long i;

	if (setting) {
		if (notShutdown) {
			long i = 0;
			while (i < numContacts) {
				MoverPtr mover = (MoverPtr)ObjectManager->get(contacts[i] & 0x7FFF);
				if (contacts[i] & VISUAL_CONTACT_FLAG) {
					master->removeContact(this, mover);
					i++;
					}
				else
					removeContact(i);
			}
		}
//		clearContacts();
		notShutdown = false;
		}
	else {
		if (!notShutdown)
			for (i = 0; i < numContacts; i++) {
				MoverPtr mover = (MoverPtr)ObjectManager->get(contacts[i] & 0x7FFF);
				if (contacts[i] & VISUAL_CONTACT_FLAG)
					master->addContact(this, mover, i, CONTACT_VISUAL);
				else
					master->addContact(this, mover, i, getSensorQuality());
			}
		notShutdown = true;
	}
}

//---------------------------------------------------------------------------

bool SensorSystem::enabled (void) {

	if (master && (masterIndex > -1)) {
		if (!owner->getExistsAndAwake())
			return(false);
		if (owner->isDisabled())
			return(false);
		if (owner->isMover()) {
			MoverPtr mover = (MoverPtr)owner;
			return((mover->sensor != 255) && !mover->inventory[mover->sensor].disabled);
		}
		return(true);
	}
	return(false);
}

//---------------------------------------------------------------------------
void SensorSystem::setRange (float newRange) {

	range = newRange;
}

//---------------------------------------------------------------------------
float SensorSystem::getRange (void) 
{
	float result = range;

	/*		NO More recon specialist
	if (owner && owner->isMover())
	{
		MoverPtr mover = (MoverPtr)owner;
		if (mover->pilot && mover->pilot->isReconSpecialist())
			result += range * 0.2f;
	}
	*/
	
	return result;
}

//---------------------------------------------------------------------------

void SensorSystem::setSkill (long newSkill) {

	skill = newSkill;
}

//---------------------------------------------------------------------------

// PORT (M8-B45): EXPERIMENT ONLY, off unless MC2_SENSOR_RANGE_SCALE is set.
//
// The headless reproduction cannot reach the owner's situation on its own: with
// nobody giving orders, no home unit ever comes within sensor range of an enemy
// VEHICLE -- the nearest approach all run is 402 world units against a range of
// 400. So "vehicles are never spotted" could be nothing but that geometry.
//
// This scales the range so the condition actually arises, and it is built to come
// back negative: if vehicles are then spotted normally, the class asymmetry was an
// artifact of distance and the search moves elsewhere; if vehicles are STILL never
// spotted while mechs are, the fault is real and class-specific.
//
// File-scope, read once (R1-19). Unset means multiply by 1, i.e. no change at all.
static const float MC2_SensorRangeScale = []() {
	const char* e = getenv("MC2_SENSOR_RANGE_SCALE");
	const float v = (e && *e) ? (float)atof(e) : 1.0f;
	return (v > 0.0f) ? v : 1.0f;
}();

float SensorSystem::getEffectiveRange (void) {

	return(range * ecmEffect * MC2_SensorRangeScale);
}

//---------------------------------------------------------------------------

// Declared here because calcContactStatus, earlier in the file, uses it.
extern const bool MC2_TeamSensorCensus;

// PORT (M8-B45): WHICH GATE REFUSES A VEHICLE?
//
// Measured so far: the home team's sensors produce contacts for enemy MECHS and
// never once for an enemy VEHICLE, across 24,000 evaluations. Eliminated already:
// the sensor gate is open, the team has sensors, index and id agree, contacts go
// to the LIVE team instance, the mover list contains every live vehicle (the ones
// it skips have exists=0 and are empty pool slots), and every live vehicle has
// OBJECT_FLAG_SENSOR.
//
// So the refusal happens inside calcContactStatus, and this tallies WHICH return
// does it, split by target class. The distance to the target is carried too --
// a vehicle refused only at long range is correct and uninteresting, and this
// probe is built so that outcome is visible rather than hidden.
enum {
	MC2_CC_MECH = 0,
	MC2_CC_VEHICLE = 1,
	MC2_CC_OTHER = 2,
	MC2_CC_CLASSES = 3
};
enum {
	MC2_CC_NO_OWNER_TEAM = 0,
	MC2_CC_REMOVED,
	MC2_CC_OFF_TERRAIN,
	MC2_CC_NO_MASTER,
	MC2_CC_OWNER_SENSOR_DEAD,
	MC2_CC_NOT_SENSED,
	MC2_CC_SPOTTED,
	MC2_CC_NO_FLAG_OR_DISABLED,
	MC2_CC_NULL_SIGNATURE,
	MC2_CC_ECM,
	MC2_CC_SHUTDOWN,
	MC2_CC_OUT_OF_RANGE,
	MC2_CC_NO_LOS,
	MC2_CC_IN_RANGE,
	MC2_CC_HAS_LOS,
	MC2_CC_EYE_NO,
	MC2_CC_EYE_YES,
	MC2_CC_REASONS
};
static long MC2_CCTally[MC2_CC_CLASSES][MC2_CC_REASONS];
static long MC2_CCNearest[MC2_CC_CLASSES][MC2_CC_REASONS];
static const char* MC2_CCReasonName[MC2_CC_REASONS] = {
	"owner has no team", "target REMOVED", "off select-terrain",
	"no master/range", "owner sensor dead", "in list but not sensed", "SPOTTED",
	"no flag / disabled", "null signature", "carries ECM",
	"status SHUTDOWN", "out of sensor range", "no line of sight",
	"IN sensor range", "HAS line of sight",
	"eye-only: unseen", "eye-only: SEEN"
};
static long MC2_CCCalls = 0;

static void MC2_CCNote (long cls, long reason, float dist)
{
	if ((cls < 0) || (cls >= MC2_CC_CLASSES) || (reason < 0) || (reason >= MC2_CC_REASONS))
		return;
	++MC2_CCTally[cls][reason];
	const long d = (long)dist;
	if ((MC2_CCTally[cls][reason] == 1) || (d < MC2_CCNearest[cls][reason]))
		MC2_CCNearest[cls][reason] = d;
}

void MC2_CCDump (void)
{
	static const char* clsName[MC2_CC_CLASSES] = { "mech", "vehicle", "other" };
	printf("[calc-contact] after %ld calls (nearest = closest target that hit that outcome)\n", MC2_CCCalls);
	for (long c = 0; c < MC2_CC_CLASSES; ++c)
		for (long r = 0; r < MC2_CC_REASONS; ++r)
			if (MC2_CCTally[c][r])
				printf("[calc-contact]   %-7s %-22s %8ld  nearest %ld world units (%ld m)\n",
				       clsName[c], MC2_CCReasonName[r], MC2_CCTally[c][r],
				       MC2_CCNearest[c][r], (long)(MC2_CCNearest[c][r] / 5.01f));
	fflush(stdout);
}

long SensorSystem::calcContactStatus (MoverPtr mover) {
	const long mc2Cls = (mover->getObjectClass() == BATTLEMECH) ? MC2_CC_MECH :
	                    ((mover->getObjectClass() == GROUNDVEHICLE) ? MC2_CC_VEHICLE : MC2_CC_OTHER);
	const float mc2Dist = owner ? owner->distanceFrom(mover->getPosition()) : 0.0f;
	const bool mc2On = MC2_TeamSensorCensus && owner && owner->getTeam() && (owner->getTeam() == Team::home);
	if (mc2On) ++MC2_CCCalls;
	if (mc2On && ((MC2_CCCalls % 20000) == 0)) MC2_CCDump();
	// The scale was missing: "out of range at 80 m" means nothing without the
	// range itself. Printed once from a home sensor.
	if (mc2On && (MC2_CCCalls == 1))
	{
		printf("[calc-contact] home sensor effective range = %.0f world units (%.0f m)\n",
			getEffectiveRange(), getEffectiveRange() / 5.01f);
		fflush(stdout);
	}

	if (!owner->getTeam())
		return(CONTACT_NONE);

	if (mover->getFlag(OBJECT_FLAG_REMOVED) && mc2On) MC2_CCNote(mc2Cls, MC2_CC_REMOVED, mc2Dist);
	if (mover->getFlag(OBJECT_FLAG_REMOVED)) {
		return(CONTACT_NONE);
	}

	//----------------------------------------------------------
	//If object we are looking for is at the edge, NO CONTACT!!
	if (!Terrain::IsGameSelectTerrainPosition(mover->getPosition()) && mc2On)
		MC2_CCNote(mc2Cls, MC2_CC_OFF_TERRAIN, mc2Dist);
	if (!Terrain::IsGameSelectTerrainPosition(mover->getPosition()))
		return CONTACT_NONE;
		
	//-------------------------------------------------------------
	// Should be properly set when active probes are implemented...
	long newContactStatus = CONTACT_NONE;
	// A ZERO-RANGE SENSOR SPOTS PURELY BY EYE, and this branch was NOT counted by
	// the tally below -- so every evaluation by the owner's tanks, which is exactly
	// the population in question, was invisible to the instrument. Counted now.
	if (!notShutdown || (range == 0.0) && !broken) 
	{
		if (owner->lineOfSight(mover) && !mover->isDisabled())
			newContactStatus = CONTACT_VISUAL;
		if (mc2On)
			MC2_CCNote(mc2Cls, (newContactStatus == CONTACT_NONE) ? MC2_CC_EYE_NO : MC2_CC_EYE_YES, mc2Dist);
		return(newContactStatus);
	}

	if (((masterIndex == -1) || (range < 0.0)) && mc2On) MC2_CCNote(mc2Cls, MC2_CC_NO_MASTER, mc2Dist);
	if ((masterIndex == -1) || (range < 0.0)) {
		return(CONTACT_NONE);
	}

	if (owner->isMover()) {
		MoverPtr mover = (MoverPtr)owner;
		if ((mover->sensor == 255) || mover->inventory[mover->sensor].disabled || broken) {
			return(CONTACT_NONE);
		}
	}

	if (mc2On && !(mover->getFlag(OBJECT_FLAG_SENSOR) && !mover->isDisabled()))
		MC2_CCNote(mc2Cls, MC2_CC_NO_FLAG_OR_DISABLED, mc2Dist);
	if (mover->getFlag(OBJECT_FLAG_SENSOR) && !mover->isDisabled()) 
	{
		bool moverNotContact = mover->hasNullSignature() || (mover->getEcmRange() != 0.0f);
		bool moverInvisible = (mover->getStatus() == OBJECT_STATUS_SHUTDOWN);
		if (mc2On)
		{
			// Split the composite condition: which of the three actually refuses a
			// vehicle? Counting all of them means a clean row clears that cause.
			if (mover->hasNullSignature()) MC2_CCNote(mc2Cls, MC2_CC_NULL_SIGNATURE, mc2Dist);
			if (mover->getEcmRange() != 0.0f) MC2_CCNote(mc2Cls, MC2_CC_ECM, mc2Dist);
			if (moverInvisible) MC2_CCNote(mc2Cls, MC2_CC_SHUTDOWN, mc2Dist);
			if (!moverInvisible && !moverNotContact)
			{
				if (owner->distanceFrom(mover->getPosition()) > getEffectiveRange())
					MC2_CCNote(mc2Cls, MC2_CC_OUT_OF_RANGE, mc2Dist);
				else
					MC2_CCNote(mc2Cls, MC2_CC_IN_RANGE, mc2Dist);
				// LOS succeeded 164 times for mech targets and ZERO times in 48,755 checks
				// for vehicle targets. Team::lineOfSight is aimed at the TARGET's
				// getLOSPosition(), which Mover raises by its lowest weapon node -- high
				// on a mech, near the ground on a tank. Report that height and the
				// target's appear radius per class; similar values would clear this.
				if ((MC2_CCTally[mc2Cls][MC2_CC_NO_LOS] % 20000) == 0)
				{
					Stuff::Vector3D lp = mover->getLOSPosition();
					printf("[los-geom] %s target '%s'  losZ-posZ=%.2f  appearRadius=%.2f  dist=%.0f\n",
						(mc2Cls == MC2_CC_MECH) ? "mech   " : "vehicle",
						mover->getName() ? mover->getName() : "?",
						lp.z - mover->getPosition().z, mover->getAppearRadius(), mc2Dist);
					fflush(stdout);
				}
				if (!(hasLOSCapability && owner->lineOfSight(mover, owner->isMover() ? 0.0f : owner->getAppearRadius())))
					MC2_CCNote(mc2Cls, MC2_CC_NO_LOS, mc2Dist);
				else
					MC2_CCNote(mc2Cls, MC2_CC_HAS_LOS, mc2Dist);
			}
		}
		if (!moverInvisible && !moverNotContact)
		{
			float distanceToMover = owner->distanceFrom(mover->getPosition());
			float sensorRange = getEffectiveRange();
			if (distanceToMover <= sensorRange)
			{
				//-------------------------------------------
				//No need to check shutdown and probe AGAIN!
				newContactStatus = getSensorQuality();

				//---------------------------------------
				// If ecm affecting me, my skill drops...
				// CUT, per Mitch 2/10/00
				if ((ecmEffect < 1.0) && (newContactStatus > CONTACT_SENSOR_QUALITY_1))
					newContactStatus--;

				//---------------------------------------------------
				// We now we are within sensor range, check visual.
				float startRadius = 0.0f;
				if (!owner->isMover())
					startRadius = owner->getAppearRadius();

				if (hasLOSCapability && owner->lineOfSight(mover,startRadius))
					newContactStatus = CONTACT_VISUAL;
			}
			else	//Still need to check if visual!!! ECM and Lookout Towers!!
			{
				float startRadius = 0.0f;
				if (!owner->isMover())
					startRadius = owner->getAppearRadius();

				if (hasLOSCapability && owner->lineOfSight(mover,startRadius))
					newContactStatus = CONTACT_VISUAL;
			}
		}
		else
		{
			//Target is shutdown, can ONLY be visual cause this platform has no probe.
			float startRadius = 0.0f;
			if (!owner->isMover())
				startRadius = owner->getAppearRadius();

			if (hasLOSCapability && owner->lineOfSight(mover,startRadius))
		    	newContactStatus = CONTACT_VISUAL;
		}
	}

	//Let us know that we can see something, sensor or otherwise!!
	if (mover->getTeam() && (owner->getTeam() == Team::home) &&	mover->getTeam()->isEnemy(Team::home) &&
		(newContactStatus != CONTACT_NONE))
	{
		SensorSystemManager::enemyInLOS = true;
	}
 
	if (mc2On)
		MC2_CCNote(mc2Cls, (newContactStatus == CONTACT_NONE) ? MC2_CC_NOT_SENSED : MC2_CC_SPOTTED, mc2Dist);
	return(newContactStatus);
}

//---------------------------------------------------------------------------

long SensorSystem::getSensorQuality (void) {

	if (owner && owner->getPilot())
	{
		if (owner->getPilot()->isSensorProfileSpecialist())
		{
			if (owner->getPilot()->getRank() < 4)
			{
				return(owner->getPilot()->getRank() + CONTACT_SENSOR_QUALITY_1);
			}
			else if (owner->getPilot()->getRank() == 4)
			{
				return(CONTACT_SENSOR_QUALITY_4);
			}
		}
	}
	
	return(CONTACT_SENSOR_QUALITY_1);
}

//---------------------------------------------------------------------------

bool SensorSystem::isContact (MoverPtr mover) {
	long i;

	if (mover->getFlag(OBJECT_FLAG_REMOVED))
		return(false);
	if (notShutdown)
		return(mover->getContactInfo()->getSensor(id) < 255);
	for (i = 0; i < numContacts; i++)
		if ((contacts[i] & 0x7FFF) == mover->getHandle())
			return(true);
	return(false);
}

//---------------------------------------------------------------------------

void SensorSystem::addContact (MoverPtr mover, bool visual) {

	Assert(!isContact(mover), 0, " SensorSystem.addContact: already contact ");
	if (numContacts < MAX_CONTACTS_PER_SENSOR) {
		contacts[numContacts] = mover->getHandle();
		if (visual)
			contacts[numContacts] |= VISUAL_CONTACT_FLAG;
		if (notShutdown)
			master->addContact(this, mover, numContacts, visual ? CONTACT_VISUAL :  getSensorQuality());
		numContacts++;
	}
}

//---------------------------------------------------------------------------

void SensorSystem::modifyContact (MoverPtr mover, bool visual) {

	long contactNum = mover->getContactInfo()->getSensor(id);
	if (contactNum < MAX_CONTACTS_PER_SENSOR)
	{
		if (visual)
			contacts[contactNum] =  mover->getHandle() | 0x8000;
		else
			contacts[contactNum] =  mover->getHandle();
	}

	if (notShutdown)
		master->modifyContact(this, mover, visual ? CONTACT_VISUAL :  getSensorQuality());
}

//---------------------------------------------------------------------------

void SensorSystem::removeContact (long contactIndex) {

	//-----------------------------------------------
	// This assumes the contactIndex is legitimate...

	MoverPtr contact = (MoverPtr)ObjectManager->get(contacts[contactIndex] & 0x7FFF);
	Assert(contact != NULL, contacts[contactIndex] & 0x7FFF, " SensorSystem.removeContact: bad contact ");
	
	numContacts--;
	if ((numContacts > 0) && (contactIndex != numContacts)) {
		//-----------------------------------------------
		// Fill vacated slot with contact in last slot...
		contacts[contactIndex] = contacts[numContacts];
		MoverPtr contact = (MoverPtr)ObjectManager->get(contacts[numContacts] & 0x7FFF);
		contact->getContactInfo()->sensors[id] = contactIndex;
	}

	if (notShutdown)
		master->removeContact(this, contact);
}

//---------------------------------------------------------------------------

void SensorSystem::removeContact (MoverPtr contact) {

	long contactIndex = contact->getContactInfo()->getSensor(id);
	if (contactIndex < 255)
		removeContact(contactIndex);
}

//---------------------------------------------------------------------------

void SensorSystem::clearContacts (void) {

	while (numContacts)
		removeContact((long)0);
}

//---------------------------------------------------------------------------

void SensorSystem::updateContacts (void) {

	if ((masterIndex == -1) || (range < 0.0))
		return;

	if (!enabled()) {
		clearContacts();
		return;
	}

	//---------------------------------------------------------------------
	// If we've already scanned this frame, don't bother updating contacts.
	// Otherwise, update contacts...
	if (scenarioTime == lastScanUpdate)
		return;

	long i = 0;
	while (i < numContacts) 
	{
		MoverPtr contact = (MoverPtr)ObjectManager->get(contacts[i] & 0x7FFF);
		long contactStatus = calcContactStatus(contact);
		if (contactStatus == CONTACT_NONE)
			removeContact(i);
		else 
		{
			contacts[i] =  contact->getHandle();
			if (contactStatus == CONTACT_VISUAL)
				contacts[i] |= 0x8000;
			modifyContact(contact, contactStatus == CONTACT_VISUAL);
			
/*			if (teamContactStatus < contactStatus) {
				//--------------------------------------------------
				// Better sensor info, so update the team sensors...
				contactInfo->contactStatus[owner->getTeamId()] = contactStatus;
				contactInfo->teamSpotter[owner->getTeamId()] = (unsigned char)owner->getHandle();
			}
*/			i++;
		}
	}
}

//---------------------------------------------------------------------------
#define CONTACT_TYPE1	0
#define CONTACT_TYPE2	1
#define CONTACT_TYPE3	2
#define CONTACT_TYPE4	3

void SensorSystem::updateScan (bool forceUpdate) {

	if (!forceUpdate)
		if ((masterIndex == -1) || (range < 0.0) || (turn < 2))
			return;

	if (!enabled()) {
		clearContacts();
		return;
	}

	if (!owner->getTeam())
		return;

	if (1/*(nextScanUpdate < scenarioTime) || forceUpdate*/) 
	{
	
		long currentScan = -1;
		if ((currentScan = scanBattlefield()) > -1)		//No returns size of largest contact.
		{
			if (owner->isMover() && 
				(owner->getTeam() == Team::home) && 
				(currentScan > SoundSystem::largestSensorContact))
			{
				SoundSystem::largestSensorContact = currentScan;
			}
		}

		lastScanUpdate = scenarioTime;
		if (!forceUpdate)
			nextScanUpdate += scanFrequency;
	}
}

//---------------------------------------------------------------------------
__inline void getLargest (long &currentLargest, MoverPtr mover, long contactStatus)
{
	long thisMoverSize = -1;
	switch (contactStatus)
	{
		case	CONTACT_SENSOR_QUALITY_1:
		case	CONTACT_SENSOR_QUALITY_2:
			thisMoverSize = 0;
			break;

		case	CONTACT_SENSOR_QUALITY_3:
		case	CONTACT_SENSOR_QUALITY_4:
		case	CONTACT_VISUAL:
			float tonnage = mover->getTonnage();
			if (tonnage < 35.0f)
				thisMoverSize = 1;
			else if (tonnage < 55.0f)
				thisMoverSize = 2;
			else if (tonnage < 75.0f)
				thisMoverSize = 3;
			else
				thisMoverSize = 4;
			break;
	}

	if (thisMoverSize > currentLargest)
		currentLargest = thisMoverSize;
}

//---------------------------------------------------------------------------
long SensorSystem::scanBattlefield (void) 
{
	long i;
	//NOW returns size of largest contact!
	long currentLargest = -1;
	
	if (!owner)
		Fatal(0, " Sensor has no owner ");

	if (!master)
		Fatal(0, " Sensor has no master ");

	if ((masterIndex == -1) || (range < 0.0))
		return(0);

	long numNewContacts = 0;

	long numMovers = ObjectManager->getNumMovers();
	for (i = 0; i < numMovers; i++) 
	{
		MoverPtr mover = (MoverPtr)ObjectManager->getMover(i);
		if (mover->getExists() && (mover->getTeamId() != owner->getTeamId())) 
		{
			long contactStatus = calcContactStatus(mover);
			if (isContact(mover)) 
			{
				if (contactStatus == CONTACT_NONE)
					removeContact(mover);
				else
				{
					modifyContact(mover, contactStatus == CONTACT_VISUAL ? true : false);
					getLargest(currentLargest,mover,contactStatus);
				}
			}
			else 
			{
				if (contactStatus != CONTACT_NONE) 
				{
					addContact(mover, contactStatus == CONTACT_VISUAL ? true : false);
					getLargest(currentLargest,mover,contactStatus);
					numNewContacts++;
				}
			}
		}
	}

	totalContacts += numNewContacts;
	
	return(currentLargest);
}

//---------------------------------------------------------------------------

long SensorSystem::scanMover (Mover* mover) {

	//---------------------------------------------------------------------
	// For now, I DO NOT return the largest contact. This should be
	// okay, since it'll just get caught during the next normal update next
	// frame.

	if (!enabled())
		return(0);

	if (mover->getExists() && (mover->getTeamId() != owner->getTeamId())) {
		long contactStatus = calcContactStatus(mover);
		if (isContact(mover)) 
		{
			if (contactStatus == CONTACT_NONE)
				removeContact(mover);
			else
			{
				modifyContact(mover, contactStatus == CONTACT_VISUAL ? true : false);
				//getLargest(currentLargest,mover,contactStatus);
			}
		}
		else 
		{
			if (contactStatus != CONTACT_NONE) 
			{
				addContact(mover, contactStatus == CONTACT_VISUAL ? true : false);
				//getLargest(currentLargest,mover,contactStatus);
				totalContacts++;
			}
		}
	}
	return(0);
}

//---------------------------------------------------------------------------

long SensorSystem::getTeamContacts (long* contactList, long contactCriteria, long sortType) {

	Assert(master != NULL, 0, " SensorSystem.getTeamContacts: null master ");
	return(master->getContacts(owner, contactList, contactCriteria, sortType));
}

//***************************************************************************
// TEAM SENSOR SYSTEM class
//***************************************************************************

void* TeamSensorSystem::operator new (size_t mySize) {

	void *result = missionHeap->Malloc(mySize);
	return(result);
}

//---------------------------------------------------------------------------

void TeamSensorSystem::operator delete (void* us) {

	missionHeap->Free(us);
}

//---------------------------------------------------------------------------

long MC2_TeamInitCalls = 0;

void TeamSensorSystem::init (void) {

	++MC2_TeamInitCalls;
	teamId = -1;
	nextContactId = 0;
	numContactUpdatesPerPass = NUM_CONTACT_UPDATES_PER_PASS;
	curContactUpdate = 0;
	numContacts = 0;
	numEnemyContacts = 0;
	numSensors = 0;
	ecms = NULL;
	jammers = NULL;
}

//---------------------------------------------------------------------------
void TeamSensorSystem::destroy (void)
{
	init();
}

//---------------------------------------------------------------------------

void TeamSensorSystem::setTeam (TeamPtr newTeam) {

	teamId = newTeam->getId();

	if (Team::teams[teamId]->rosterSize < NUM_CONTACT_UPDATES_PER_PASS)
		numContactUpdatesPerPass = Team::teams[teamId]->rosterSize;
	else
		numContactUpdatesPerPass = NUM_CONTACT_UPDATES_PER_PASS;
}

//---------------------------------------------------------------------------

void TeamSensorSystem::addSensor (SensorSystemPtr sensor) {

	if (numSensors == MAX_SENSORS_PER_TEAM)
		Fatal(0, " TeamSensorSystem.addSensor: too many sensors ");

	sensor->setMasterIndex(numSensors);
	sensors[numSensors++] = sensor;
}

//---------------------------------------------------------------------------

void TeamSensorSystem::removeSensor (SensorSystemPtr sensor) {

	sensor->clearContacts();
	long index = sensor->getMasterIndex();
	sensor->setMasterIndex(-1);

	sensors[index] = NULL;
	if (index < (numSensors - 1)) {
		sensors[index] = sensors[numSensors - 1];
		sensors[index]->setMasterIndex(index);
		sensors[numSensors - 1] = NULL;
	}
	numSensors--;
}

//---------------------------------------------------------------------------

// PORT (M8-B45): WHY DOES A RUNNING SENSOR SYSTEM PRODUCE NO CONTACTS?
//
// Established by measurement before this probe was written, on the owner's own
// mc2_15 quicksave loaded headless:
//   * the sensor gate in mission.cpp is OPEN -- 1,500 SensorManager->update()
//     calls, zero skips, useSensors=1 isPaused=0. So "the harness never runs
//     sensors" is ELIMINATED.
//   * and yet 24,000 enemy-vehicle evaluations all read CONTACT_NONE, including
//     enemies 36 m from a player unit.
//
// Every scan below is gated on `numSensors > 0`, and a team with no registered
// sensors does nothing here SILENTLY -- no scan, no contacts, no warning. That
// is the next thing to rule in or out, and it is falsifiable: this prints the
// per-team sensor and contact counts, so a healthy home team with many sensors
// and still no contacts would clear it and send the search deeper, into
// updateScan itself.
//
// File-scope and read once (R1-19): a getenv in a per-frame path moved the
// strict bar's hash before.
extern const bool MC2_TeamSensorCensus;
const bool MC2_TeamSensorCensus = []() {
	const char* e = getenv("MC2_GV_FACE_CENSUS");
	return e && *e && strcmp(e, "0") != 0;
}();
// PER-TEAM counters. The first version of this counted calls GLOBALLY and
// sampled every 400th, which aliased against the one-team-per-frame
// round-robin and reported only team 1 -- an instrument lying by
// construction. Counted per team, every team reports.
static long MC2_TeamSensorCalls[MAX_TEAMS];
// Sampling every 200th scan is too thin to say "this team never has contacts":
// contacts come and go between samples. These carry the PEAK over the whole run
// and the number of times addContact actually fired, which a sample cannot miss.
long MC2_TeamContactPeak[MAX_TEAMS];
long MC2_TeamAddContacts[MAX_TEAMS];
long MC2_TeamResets[MAX_TEAMS];
// Closest-approach tracking, indexed by mover handle (handles run 3..210 against
// a maxMovers of 338, measured).
#define MC2_CLOSE_MAX 512
static float MC2_CloseDist[MC2_CLOSE_MAX];
static float MC2_CloseRange[MC2_CLOSE_MAX];
static long MC2_CloseStatus[MC2_CLOSE_MAX];
static long MC2_CloseEverSeen[MC2_CLOSE_MAX];
static long MC2_CloseIsVehicle[MC2_CLOSE_MAX];
static long MC2_CloseDisabled[MC2_CLOSE_MAX];
static long MC2_CloseDestroyed[MC2_CLOSE_MAX];
static long MC2_CloseFlag[MC2_CLOSE_MAX];
static long MC2_CloseClass[MC2_CLOSE_MAX];
// COPIES, not pointers. The first version stored getName() pointers, and the
// save loads destroy and rebuild every mover, so by the time they were printed
// they were dangling and the output was garbage. An instrument that lies.
static char MC2_CloseName[MC2_CLOSE_MAX][32];
static char MC2_CloseSeer[MC2_CLOSE_MAX][32];
static const bool MC2_CloseTest = []() {
	const char* e = getenv("MC2_CLOSE_TEST");
	return e && *e && strcmp(e, "0") != 0;
}();
static bool MC2_CloseTestDone = false;
static bool MC2_CloseTestArmed = false;
static const bool MC2_CloseInit = []() {
	for (long i = 0; i < MC2_CLOSE_MAX; ++i) MC2_CloseDist[i] = 1.0e9f;
	return true;
}();
// numContacts fell from a peak of 10 to 0 with ZERO removals counted, and the
// only other writer of numContacts is TeamSensorSystem::init(). Either init is
// running mid-mission, or update() and addContact() are running on DIFFERENT
// TeamSensorSystem objects for the same teamId -- in which case the per-teamId
// counters above have been merging two instances all along. The object address
// settles it, and equal addresses kill the second explanation outright.
void* MC2_TeamUpdateThis[MAX_TEAMS];
void* MC2_TeamAddThis[MAX_TEAMS];
long MC2_AddToLive[MAX_TEAMS];
long MC2_AddToOrphan[MAX_TEAMS];

long MC2_AddStatusHist[MAX_TEAMS][NUM_CONTACT_STATUSES];

void TeamSensorSystem::update (void) {
	long i;

	if (MC2_TeamSensorCensus && (teamId >= 0) && (teamId < MAX_TEAMS))
	{
		++MC2_TeamSensorCalls[teamId];
		MC2_TeamUpdateThis[teamId] = (void*)this;
		// THE MISSING SCALE. calcContactStatus compares distance against
		// getEffectiveRange(), and the first home sensor sampled reported a range of
		// ONE world unit. If home sensor ranges are near zero, almost nothing can
		// ever be detected and the mech/vehicle split is incidental. Printed once per
		// team, for every sensor, with the owner named -- sane ranges clear this.
		// Sampled at call 1 AND at call 601: the call-1 values are pre-init and say
		// nothing about the running mission.
		// PORT (M8-B45): THE EXPERIMENT THE HARNESS COULD NOT OTHERWISE RUN.
		// Off unless MC2_CLOSE_TEST is set.
		//
		// The owner reports that running away and running back does NOT reveal the
		// invisible tanks. That kills the innocent explanation -- "they are simply
		// beyond the 400 m sensor bubble" -- because closing the distance should then
		// have revealed them. Headless we issue no orders, so no living unit with a
		// working sensor ever approaches a Storm or a Legion; the deciding geometry
		// never arises on its own.
		//
		// So it is created once: the first living home mech with an enabled sensor is
		// placed 100 m from the nearest enemy GROUND VEHICLE, and the closest-approach
		// census then says whether that tank becomes a contact. Spotted means the
		// sensor code is sound and the owner's report needs a different cause;
		// unspotted at 100 m inside a 400 m sensor is the defect, isolated.
		// Prove the experiment ran. Its first attempt printed nothing at all, and a
		// silent experiment proves nothing either way.
		if (MC2_CloseTest && !MC2_CloseTestArmed && (Team::teams[teamId] == Team::home))
		{
			MC2_CloseTestArmed = true;
			printf("[close-test] armed: home roster %ld\n", (long)Team::home->getRosterSize());
			fflush(stdout);
		}

		if (MC2_CloseTest && !MC2_CloseTestDone && (Team::teams[teamId] == Team::home)
		    && (MC2_TeamSensorCalls[teamId] > 30))
		{
			for (long r = 0; (r < Team::home->getRosterSize()) && !MC2_CloseTestDone; ++r)
			{
				MoverPtr hm = Team::home->getMover(r);
				if (!hm || !hm->getExists() || hm->isDisabled()) continue;
				SensorSystem* ss = hm->getSensorSystem();
				if (!ss || !ss->enabled() || (ss->getEffectiveRange() < 100.0f)) continue;

				MoverPtr target = NULL;
				float best = 1.0e9f;
				for (long v = 0; v < ObjectManager->getNumMovers(); ++v)
				{
					MoverPtr em = ObjectManager->getMover(v);
					if (!em || !em->getExists() || em->isDisabled()) continue;
					if (em->getObjectClass() != GROUNDVEHICLE) continue;
					if (em->getTeamId() == Team::home->getId()) continue;
					const float d = hm->distanceFrom(em->getPosition());
					if (d < best) { best = d; target = em; }
				}
				if (!target) continue;

				Stuff::Vector3D p = target->getPosition();
				p.x += 100.0f;
				p.z = land->getTerrainElevation(p);
				hm->setPosition(p);
				MC2_CloseTestDone = true;
				printf("[close-test] moved '%s' (sensor %.0f m) next to '%s' -- was %.0f m away, now %.0f m\n",
				       hm->getName() ? hm->getName() : "?", ss->getEffectiveRange(),
				       target->getName() ? target->getName() : "?", best,
				       hm->distanceFrom(target->getPosition()));
				fflush(stdout);
			}
		}

		// PORT (M8-B45): IS ANY LIVE UNIT MISSING FROM THE MOVER LIST?
		//
		// The owner's log caught the shooter at last:
		//   [hit] 'Anubis' was hit by 'Storm' class=3 distance=50 m contactStatus=0
		// A tank at FIFTY METRES, inside a 400 m sensor and inside the 225 m eye
		// range, reading CONTACT_NONE. And in the same session both the contact tally
		// and the closest-approach census say no vehicle came nearer than 293-399 m.
		//
		// Those two censuses and SensorSystem::scanBattlefield all walk
		// ObjectManager's moverList. A unit that is not in that list is never scanned
		// by anything, so it can never become a contact -- while it still updates and
		// still fires, because those run off the vehicles/mechs arrays instead.
		// Silently invisible, unblippable, untargetable, and shooting.
		//
		// So walk the real arrays and name anything alive that the list is missing.
		// An empty report clears this and sends the search back to the scan filters.
		if (MC2_TeamSensorCensus && (Team::teams[teamId] == Team::home)
		    && ((MC2_TeamSensorCalls[teamId] % 200) == 5))
		{
			// WALK THE WHOLE OBJECT LIST, not the mechs/vehicles arrays.
			// The owner's log settled it: the Storm that shot them from 50 m reports
			// inMoverList=0 while team is set, not removed, not disabled, on terrain,
			// sensor flag set. The earlier version of this census walked mechs[] and
			// vehicles[] and reported "0 missing" -- but a unit dropped from THOSE
			// arrays is invisible to that walk too, so its zero proved nothing about
			// this failure. objList holds every live object regardless.
			long missing = 0, checked = 0, orphans = 0;
			for (long o = 0; o < ObjectManager->getMaxObjects(); ++o)
			{
				GameObjectPtr go = ObjectManager->objList[o];
				if (!go || !go->getExists() || !go->isMover()) continue;
				MoverPtr mv = (MoverPtr)go;
				bool inList = false;
				for (long q = 0; q < ObjectManager->getNumMovers(); ++q)
					if (ObjectManager->getMover(q) == mv) { inList = true; break; }
				if (inList) continue;
				++orphans;
				printf("[orphan-mover] LIVE '%s' class=%ld is in NO mover list -- team=%s teamId=%ld destroyed=%d disabled=%d removed=%d\n",
					mv->getName() ? mv->getName() : "?", (long)mv->getObjectClass(),
					mv->getTeam() ? "set" : "NULL", (long)mv->getTeamId(),
					mv->isDestroyed() ? 1 : 0, mv->isDisabled() ? 1 : 0,
					mv->getFlag(OBJECT_FLAG_REMOVED) ? 1 : 0);
			}
			printf("[orphan-mover] %ld live movers in objList are in NO list\n", orphans);
			for (long pass = 0; pass < 2; ++pass)
			{
				const long n = pass ? ObjectManager->getNumVehicles() : ObjectManager->getNumMechs();
				for (long i = 0; i < n; ++i)
				{
					MoverPtr mv = pass ? (MoverPtr)ObjectManager->getVehicle(i)
					                   : (MoverPtr)ObjectManager->getMech(i);
					if (!mv || !mv->getExists()) continue;
					++checked;
					bool inList = false;
					for (long q = 0; q < ObjectManager->getNumMovers(); ++q)
						if (ObjectManager->getMover(q) == mv) { inList = true; break; }
					if (inList) continue;
					++missing;
					printf("[not-in-list] LIVE %s '%s' is NOT in the mover list -- team=%s teamId=%ld cmdr=%ld destroyed=%d disabled=%d removed=%d\n",
					       pass ? "vehicle" : "mech",
					       mv->getName() ? mv->getName() : "?",
					       mv->getTeam() ? "set" : "NULL",
					       (long)mv->getTeamId(), (long)mv->getCommanderId(),
					       mv->isDestroyed() ? 1 : 0, mv->isDisabled() ? 1 : 0,
					       mv->getFlag(OBJECT_FLAG_REMOVED) ? 1 : 0);
				}
			}
			printf("[not-in-list] %ld live movers checked, %ld missing from the list (list holds %ld)\n",
			       checked, missing, (long)ObjectManager->getNumMovers());
			fflush(stdout);
		}

		// PORT (M8-B45): CLOSEST APPROACH, TRACKED CONTINUOUSLY.
		//
		// The previous version of this table sampled at one instant, 600 sensor
		// updates after the load, and reported the nearest enemy vehicle at 677 m.
		// The owner says their mechs are within FIRING distance of an invisible
		// enemy, which that sampling cannot see: units carry on their saved orders
		// after a load, so by call 601 everyone has moved. A single sample was the
		// wrong instrument.
		//
		// This tracks, for every enemy mover of ANY class, the CLOSEST it ever comes
		// to a home unit that is alive and whose sensor is actually enabled, and the
		// contact status home held at that moment. It cannot miss the moment, and it
		// is falsifiable both ways: an enemy that closes inside sensor range while
		// reading CONTACT_NONE is the defect with a number on it, and a run where
		// every close enemy is spotted clears the whole theory.
		if (MC2_TeamSensorCensus && (Team::teams[teamId] == Team::home))
		{
			for (long v = 0; v < ObjectManager->getNumMovers(); ++v)
			{
				MoverPtr em = ObjectManager->getMover(v);
				if (!em || !em->getExists()) continue;
				if (em->getTeamId() == Team::home->getId()) continue;
				const long h = (long)em->getHandle();
				if ((h < 0) || (h >= MC2_CLOSE_MAX)) continue;

				for (long r = 0; r < Team::home->getRosterSize(); ++r)
				{
					MoverPtr hm = Team::home->getMover(r);
					if (!hm || !hm->getExists() || hm->isDisabled()) continue;
					SensorSystem* ss = hm->getSensorSystem();
					if (!ss || !ss->enabled()) continue;
					const float d = hm->distanceFrom(em->getPosition());
					if (d < MC2_CloseDist[h])
					{
						MC2_CloseDist[h] = d;
						MC2_CloseStatus[h] = (long)em->getContactStatus(Team::home->getId(), true);
						MC2_CloseRange[h] = (ss && ss->enabled()) ? ss->getEffectiveRange() : 0.0f;
						strncpy(MC2_CloseName[h], em->getName() ? em->getName() : "?", 31);
						MC2_CloseName[h][31] = 0;
						strncpy(MC2_CloseSeer[h], hm->getName() ? hm->getName() : "?", 31);
						MC2_CloseSeer[h][31] = 0;
						MC2_CloseIsVehicle[h] = (em->getObjectClass() == GROUNDVEHICLE) ? 1 : 0;
						// What is different about the one enemy that comes inside sensor range and is
						// still never spotted? calcContactStatus skips its whole sensing block for a
						// target that is disabled, so record that first.
						MC2_CloseDisabled[h] = em->isDisabled() ? 1 : 0;
						MC2_CloseDestroyed[h] = em->isDestroyed() ? 1 : 0;
						MC2_CloseFlag[h] = em->getFlag(OBJECT_FLAG_SENSOR) ? 1 : 0;
						MC2_CloseClass[h] = (long)em->getObjectClass();
					}
					if (em->getContactStatus(Team::home->getId(), true) != CONTACT_NONE)
						MC2_CloseEverSeen[h] = 1;
				}
			}

			// Every 60 home scans, about two seconds of play. The owner's attacker drives
			// up and opens fire within seconds of the save starting, so a 13-second
			// reporting interval could miss the whole event in a short playtest.
			if ((MC2_TeamSensorCalls[teamId] % 60) == 0)
			{
				printf("[close] enemies that came within 300 m of a LIVING home unit:\n");
				for (long h = 0; h < MC2_CLOSE_MAX; ++h)
				{
					if (MC2_CloseDist[h] > 300.0f) continue;   // firing range is 225 m

					printf("[close]   %-7s '%s' closed to %.0f m of '%s' (sensor %.0f m)  status then=%ld  ever spotted=%s  disabled=%ld destroyed=%ld sensorFlag=%ld class=%ld%s\n",
					       MC2_CloseIsVehicle[h] ? "vehicle" : "mech/other",
					       MC2_CloseName[h] ? MC2_CloseName[h] : "?",
					       MC2_CloseDist[h],
					       MC2_CloseSeer[h] ? MC2_CloseSeer[h] : "?",
					       MC2_CloseRange[h], MC2_CloseStatus[h],
					       MC2_CloseEverSeen[h] ? "yes" : "NO",
					       MC2_CloseDisabled[h], MC2_CloseDestroyed[h], MC2_CloseFlag[h], MC2_CloseClass[h],
					       ((MC2_CloseDist[h] <= MC2_CloseRange[h]) && !MC2_CloseEverSeen[h])
					           ? "   <-- INSIDE SENSOR RANGE AND NEVER SPOTTED" : "");
				}
				fflush(stdout);
			}
		}

		// Sampled BEFORE the save load (call 201) and AFTER it (call 601). A range
		// that is right before and wrong after pins it on the load path; unchanged
		// values move the search back to mission start.
		if ((MC2_TeamSensorCalls[teamId] == 2) || (MC2_TeamSensorCalls[teamId] == 201) || (MC2_TeamSensorCalls[teamId] == 601))
		{
			for (long q = 0; q < numSensors; ++q)
			{
				SensorSystemPtr sp = sensors[q];
				if (!sp || !sp->owner) continue;
				MoverPtr om = sp->owner->isMover() ? (MoverPtr)sp->owner : NULL;
				printf("[sensor-range] call %ld team %ld owner '%s' range=%.1f broken=%d notShutdown=%d LOS=%d  sensorIdx=%ld masterID=%ld\n",
					MC2_TeamSensorCalls[teamId], (long)teamId,
					sp->owner->getName() ? sp->owner->getName() : "?",
					sp->getRange(), sp->broken ? 1 : 0, sp->notShutdown ? 1 : 0,
					sp->hasLOSCapability ? 1 : 0,
					om ? (long)om->sensor : -1,
					(om && (om->sensor != 255)) ? (long)om->inventory[om->sensor].masterID : -1);
				// MasterComponent::stats is a UNION and getSensorRange() reads the sensor
				// member without checking the component form -- an ECM or probe in that
				// slot would return whatever its own field holds. Print the form: 2 is
				// COMPONENT_FORM_SENSOR, and a 2 here clears this explanation.
				if (om && (om->sensor != 255))
					printf("[sensor-range]      component %ld form=%ld (SENSOR=%ld)\n",
						(long)om->inventory[om->sensor].masterID,
						(long)MasterComponent::masterList[om->inventory[om->sensor].masterID].getForm(),
						(long)COMPONENT_FORM_SENSOR);
			}
			fflush(stdout);
		}
		if (numContacts > MC2_TeamContactPeak[teamId])
			MC2_TeamContactPeak[teamId] = numContacts;
		if ((MC2_TeamSensorCalls[teamId] % 200) == 1)
		{
			printf("[team-sensor] team %ld%s call %ld: numSensors=%ld numContacts=%ld rosterSize=%ld\n",
			       (long)teamId,
			       (Team::teams[teamId] == Team::home) ? " (HOME)" : "",
			       MC2_TeamSensorCalls[teamId],
			       (long)numSensors, (long)numContacts,
			       (long)Team::teams[teamId]->rosterSize);
			printf("[team-sensor]   peak numContacts=%ld addContact calls=%ld\n",
			       MC2_TeamContactPeak[teamId], MC2_TeamAddContacts[teamId]);
			// addContact WRITES contactStatus[teamId] -- the team INDEX -- while the
			// readers, GroundVehicle::update among them, index by Team::home->getId().
			// If those numberings disagree the write and the read land in different
			// slots, which is exactly "contacts exist and every reader sees NONE".
			// Measured rather than assumed; equal values clear the theory outright.
			printf("[team-sensor]   index=%ld teams[index]->getId()=%ld homeId=%ld\n",
			       (long)teamId, (long)Team::teams[teamId]->getId(),
			       Team::home ? (long)Team::home->getId() : -1);
			printf("[team-sensor]   resets=%ld  addContact status histogram:",
			       MC2_TeamResets[teamId]);
			for (long h = 0; h < NUM_CONTACT_STATUSES; ++h)
				if (MC2_AddStatusHist[teamId][h])
					printf(" status%ld=%ld", h, MC2_AddStatusHist[teamId][h]);
			printf("\n");
			printf("[team-sensor]   update this=%p  addContact this=%p  init calls=%ld\n",
			       MC2_TeamUpdateThis[teamId], MC2_TeamAddThis[teamId], MC2_TeamInitCalls);
			printf("[team-sensor]   addContact -> LIVE instance %ld, ORPHANED instance %ld\n",
			       MC2_AddToLive[teamId], MC2_AddToOrphan[teamId]);
			fflush(stdout);
		}
	}

	if (numSensors > 0) {
#if 0
		//DEBUGGING
		for (long k = 0; k < numContacts; k++) {
			MoverPtr mover = (MoverPtr)ObjectManager->get(contacts[k]);
			Assert(mover->getContactInfo()->teams[teamId] == k, 0, " Bad teams/contact link ");
		}
#endif
		
		if (Team::teams[teamId] == Team::home)
			SoundSystem::largestSensorContact = -1;

		//---------------------------------
		// First, update actual scanning...
		for (i = 0; i < numSensors; i++)
			sensors[i]->updateScan();

		if (Team::teams[teamId]->rosterSize < NUM_CONTACT_UPDATES_PER_PASS)
			numContactUpdatesPerPass = Team::teams[teamId]->rosterSize;
		else
			numContactUpdatesPerPass = NUM_CONTACT_UPDATES_PER_PASS;

		//--------------------------------
		// Now, update current contacts...
		for (i = 0; i < numContactUpdatesPerPass; i++) {
			if (curContactUpdate >= numSensors)
				curContactUpdate = 0;
			sensors[curContactUpdate]->updateContacts();
			curContactUpdate++;
		}
	}
}

//---------------------------------------------------------------------------

long TeamSensorSystem::getVisualContacts (MoverPtr* moverList) {
	long i;

	long numVisualContacts = 0;
	for (i = 0; i < numContacts; i++) {
		MoverPtr contact = (MoverPtr)ObjectManager->get(contacts[i]);
		if (!contact->getFlag(OBJECT_FLAG_REMOVED))
			if (contact->getContactStatus(teamId, true) == CONTACT_VISUAL)
				moverList[numVisualContacts++] = contact;
	}
	return(numVisualContacts);
}

//---------------------------------------------------------------------------

long TeamSensorSystem::getSensorContacts (MoverPtr* moverList) {
	long i;

	static bool isSensor[NUM_CONTACT_STATUSES] = {false, true, true, true, true, false};
	long numSensorContacts = 0;
	for (i = 0; i < numContacts; i++) {
		MoverPtr contact = (MoverPtr)ObjectManager->get(contacts[i]);
		if (!contact->getFlag(OBJECT_FLAG_REMOVED))
			if (isSensor[contact->getContactStatus(teamId, true)])
				moverList[numSensorContacts++] = contact;
	}
	return(numSensorContacts);
}

//---------------------------------------------------------------------------

bool TeamSensorSystem::meetsCriteria (GameObjectPtr looker, MoverPtr mover, long contactCriteria) {

	bool isSensor[NUM_CONTACT_STATUSES] = {false, true, true, true, true, false};
	long status = mover->getContactStatus(teamId, true);

	if (mover->getFlag(OBJECT_FLAG_REMOVED))
		return(false);

	if (status == CONTACT_NONE)
		return(false);

	if (contactCriteria & CONTACT_CRITERIA_VISUAL_OR_SENSOR)
		if ((status != CONTACT_VISUAL) && !isSensor[status])
			return(false);

	if (contactCriteria & CONTACT_CRITERIA_VISUAL)
		if (status != CONTACT_VISUAL)
			return(false);

	if (contactCriteria & CONTACT_CRITERIA_SENSOR)
		if (!isSensor[status])
			return(false);

	if (contactCriteria & CONTACT_CRITERIA_NOT_CHALLENGED) {
		if (mover->getChallenger() != NULL)
			return(false);
	}

	if (contactCriteria & CONTACT_CRITERIA_NOT_DISABLED) {
		if (mover->isDisabled())
			return(false);
	}

	if (contactCriteria & CONTACT_CRITERIA_ENEMY) {
		if ((teamId == -1) || !mover->getTeam())
			return(false);
		if (!Team::teams[teamId]->isEnemy(mover->getTeam()))
			return(false);
	}

	if (contactCriteria & CONTACT_CRITERIA_ARMED) {
		if (mover->numFunctionalWeapons == 0)
			return(false);
	}

	return(true);
}

//---------------------------------------------------------------------------

bool TeamSensorSystem::hasSensorContact (long teamID) {
	long i;

	for (i = 0; i < numContacts; i++) {
		MoverPtr mover = (MoverPtr)ObjectManager->get(contacts[i]);
		if (!mover->getFlag(OBJECT_FLAG_REMOVED)) {
			static bool isSensor[NUM_CONTACT_STATUSES] = {false, true, true, true, true, false};
			if (isSensor[mover->getContactStatus(teamID, true)])
				return(true);
		}
	}
	return(false);
}

//---------------------------------------------------------------------------

long TeamSensorSystem::getContacts (GameObjectPtr looker, long* contactList, long contactCriteria, long sortType) {
	long contact;
	long i;

	if ((sortType != CONTACT_SORT_NONE) && !looker)
		return(0);

	static float sortValues[MAX_CONTACTS_PER_SENSOR];

	float CV = 0;
	long numValidContacts = 0;
	long handleList[MAX_CONTACTS_PER_SENSOR];
	for (i = 0; i < numContacts; i++) {
		MoverPtr mover = (MoverPtr)ObjectManager->get(contacts[i]);
		if (!meetsCriteria(looker, mover, contactCriteria))
			continue;
		handleList[numValidContacts] = mover->getHandle();
		switch (sortType) {
			case CONTACT_SORT_NONE:
				sortValues[numValidContacts] = 0.0;
				break;
			case CONTACT_SORT_CV:
				CV = (float)mover->getCurCV();
				sortValues[numValidContacts] = CV;
				break;
			case CONTACT_SORT_DISTANCE:
				sortValues[numValidContacts] = looker->distanceFrom(mover->getPosition());
				break;
		}
		numValidContacts++;
	}

	if ((numValidContacts > 0) && (sortType != CONTACT_SORT_NONE)) {
		//---------------------------------------------------------
		// BIG ASSUMPTION HERE: That a mech will not have more than
		// MAX_CONTACTS_PER_SENSOR contacts.
		if (!SensorSystem::sortList) {
			SensorSystem::sortList = new SortList;
			if (!SensorSystem::sortList)
				Fatal(0, " Unable to create Contact sortList ");
			SensorSystem::sortList->init(MAX_CONTACTS_PER_SENSOR);
		}
		bool descendSort = true;
		if (sortType == CONTACT_SORT_DISTANCE)
			descendSort = false;
		SensorSystem::sortList->clear(descendSort);
		for (contact = 0; contact < numValidContacts; contact++) {
			SensorSystem::sortList->setId(contact, handleList[contact]);
			SensorSystem::sortList->setValue(contact, sortValues[contact]);
		}
		SensorSystem::sortList->sort(descendSort);
		for (contact = 0; contact < numValidContacts; contact++)
			contactList[contact] = SensorSystem::sortList->getId(contact);
		}
	else if (contactList)
		for (contact = 0; contact < numValidContacts; contact++)
			contactList[contact] = handleList[contact];


	return(numValidContacts);
}

//---------------------------------------------------------------------------

long TeamSensorSystem::getContactStatus (MoverPtr mover, bool includingAllies) {

	if (mover->getFlag(OBJECT_FLAG_REMOVED))
		return(CONTACT_NONE);
	return(mover->getContactInfo()->getContactStatus(Team::teams[teamId]->getId(), true));
}

//---------------------------------------------------------------------------

void TeamSensorSystem::scanBattlefield (void) {
	long i;

	if (numSensors)
		for (i = 0; i < numSensors; i++)
			sensors[i]->updateScan(true);
}

//---------------------------------------------------------------------------

void TeamSensorSystem::scanMover (Mover* mover) {
	long i;

	if (numSensors)
		for (i = 0; i < numSensors; i++)
			sensors[i]->scanMover(mover);
}

//---------------------------------------------------------------------------

void TeamSensorSystem::incNumEnemyContacts (void) {

	numEnemyContacts++;
	if ((Team::teams[teamId] == Team::home) && numEnemyContacts)
		homeTeamInContact = true;
}

//---------------------------------------------------------------------------

void TeamSensorSystem::decNumEnemyContacts (void) {

	numEnemyContacts--;
	if (!numEnemyContacts && (Team::teams[teamId] == Team::home))
		homeTeamInContact = false;
		
	if (numEnemyContacts < 0)
		Fatal(0, " Negative Team Contact Count ");
}

//---------------------------------------------------------------------------

void TeamSensorSystem::addContact (SensorSystemPtr sensor, MoverPtr contact, long contactIndex, long contactStatus) {

	if (MC2_TeamSensorCensus && (teamId >= 0) && (teamId < MAX_TEAMS))
	{
		++MC2_TeamAddContacts[teamId];
		MC2_TeamAddThis[teamId] = (void*)this;
		// THE DEFECT CONDITION, MEASURED DIRECTLY: is the object taking this
		// contact still the one registered for the team? If not, the contact is
		// being filed into an orphan and no reader will ever see it. Counting both
		// outcomes means a healthy run says so instead of saying nothing.
		if (SensorManager && (SensorManager->getTeamSensor(teamId) == this))
			++MC2_AddToLive[teamId];
		else
			++MC2_AddToOrphan[teamId];

		// The orphan theory is DEAD -- every add goes to the live instance. So the
		// writes and the reads must be landing on different ContactInfo objects, or
		// on different movers entirely. Name the mover being contacted and print the
		// very ContactInfo address being written, so it can be compared with the
		// address the vehicle reads from a moment later.
		if (teamId == 0)
		{
			static char seen[64][32];
			static long seenN = 0;
			const char* nm = contact ? contact->getName() : "?";
			bool known = false;
			for (long q = 0; q < seenN; ++q)
				if (strncmp(seen[q], nm ? nm : "?", 31) == 0)
					known = true;
			if (!known && (seenN < 64))
			{
				strncpy(seen[seenN], nm ? nm : "?", 31);
				seen[seenN][31] = 0;
				++seenN;
				printf("[contact-write] HOME spots '%s' status %ld  contactInfo=%p\n",
				       nm ? nm : "?", contactStatus, (void*)contact->getContactInfo());
				fflush(stdout);
			}
		}
		// WHICH STATUS is a new contact given? addContact only ever RAISES
		// contactStatus, so a scan that classifies everything as CONTACT_NONE
		// produces exactly the observed state: contacts added, every read NONE.
		if ((contactStatus >= 0) && (contactStatus < NUM_CONTACT_STATUSES))
			++MC2_AddStatusHist[teamId][contactStatus];
	}

	Assert(numContacts < MAX_CONTACTS_PER_TEAM, numContacts, " TeamSensorSystem.addContact: max team contacts ");
	ContactInfoPtr contactInfo = contact->getContactInfo();
	contactInfo->sensors[sensor->id] = contactIndex;
	contactInfo->contactCount[teamId]++;
	if (contactInfo->contactStatus[teamId] < contactStatus) {
		contactInfo->contactStatus[teamId] = contactStatus;
		contactInfo->teamSpotter[teamId] = sensor->owner->getHandle();

	}
	if (contactInfo->contactCount[teamId] == 1) {
		contacts[numContacts] = contact->getHandle();
		contactInfo->teams[teamId] = numContacts;
		numContacts++;
		sensor->numExclusives++;
	}
}

//---------------------------------------------------------------------------

SensorSystemPtr TeamSensorSystem::findBestSpotter (MoverPtr contact, long* status) {
	long i;

	ContactInfoPtr contactInfo = contact->getContactInfo();
	if (!contactInfo) {
		char s[256];
		sprintf(s, "TeamSensorSystem.findBestSpotter: NULL contactInfo for objClass %d partID %d team %d", contact->getObjectClass(), contact->getPartId(), contact->getTeamId());
		STOP((s));
	}
	SensorSystemPtr bestSensor = NULL;
	long bestStatus = CONTACT_NONE;
	for (i = 0; i < MAX_SENSORS; i++)
		if (contactInfo->sensors[i] != 255) {
			SensorSystemPtr sensor = SensorManager->getSensor(i);
			if (sensor && sensor->owner && (teamId == sensor->owner->getTeamId())) {
				long status = sensor->calcContactStatus(contact);
				if (status >= bestStatus) {
					bestSensor = sensor;
					bestStatus = status;
				}
			}
		}
	if (status)
		*status = bestStatus;
	return(bestSensor);
}

//---------------------------------------------------------------------------

void TeamSensorSystem::modifyContact (SensorSystemPtr sensor, MoverPtr contact, long contactStatus) {

	ContactInfoPtr contactInfo = contact->getContactInfo();
	if (contactInfo->teamSpotter[teamId] == sensor->owner->getHandle()) {
		long curStatus = contactInfo->contactStatus[teamId];
		if (contactStatus > curStatus)
			contactInfo->contactStatus[teamId] = contactStatus;
		else if (contactStatus < curStatus) {
			long bestStatus;
			SensorSystemPtr bestSensor = findBestSpotter(contact, &bestStatus);
			contactInfo->contactStatus[teamId] = bestStatus;
			contactInfo->teamSpotter[teamId] = bestSensor->owner->getHandle();
		}
		}
	else {
		long curStatus = contactInfo->contactStatus[teamId];
		if (contactStatus > curStatus) {
			contactInfo->contactStatus[teamId] = contactStatus;
			contactInfo->teamSpotter[teamId] = sensor->owner->getHandle();
		}
	}
}

//---------------------------------------------------------------------------

void TeamSensorSystem::removeContact (SensorSystemPtr sensor, MoverPtr contact) {

	ContactInfoPtr contactInfo = contact->getContactInfo();
	contactInfo->sensors[sensor->id] = 255;
	contactInfo->contactCount[teamId]--;
	Assert(contactInfo->contactCount[teamId] != 255, 0, "FUDGE");

	if (contactInfo->contactCount[teamId] == 0) {
		long contactIndex = contactInfo->teams[teamId];
		Assert(contactIndex < 0xFFFF, contactIndex, " 0xFFFF ");
		MoverPtr removedContact = (MoverPtr)ObjectManager->get(contacts[contactIndex]);
		Assert(removedContact == contact, contactIndex, " TeamSensorSystem.removeContact: bad contact ");
		
		contactInfo->teams[teamId] = 0xFFFF;
		// The other way the observed state arises: contacts raised and then
		// torn straight back down, so every sampled read lands on the reset.
		if (MC2_TeamSensorCensus && (teamId >= 0) && (teamId < MAX_TEAMS))
			++MC2_TeamResets[teamId];
		contactInfo->contactStatus[teamId] = CONTACT_NONE;
		contactInfo->teamSpotter[teamId] = 0;         
	
		numContacts--;
		if ((numContacts > 0) && (contactIndex != numContacts)) {
			//-----------------------------------------------
			// Fill vacated slot with contact in last slot...
			contacts[contactIndex] = contacts[numContacts];
			MoverPtr mover = (MoverPtr)ObjectManager->get(contacts[numContacts]);
			mover->getContactInfo()->teams[teamId] = contactIndex;
		}
		}
	else if (contactInfo->teamSpotter[teamId] == sensor->owner->getHandle()) {
		if (sensor->owner->getObjectClass() == BATTLEMECH)
			Assert(sensor != NULL, 0, " dumb ");
		long bestStatus;
		SensorSystemPtr bestSensor = findBestSpotter(contact, &bestStatus);
//		Assert(bestSensor != NULL, 0, " hit ");
		if (bestSensor) {
			contact->contactInfo->contactStatus[teamId] = bestStatus;
			contact->contactInfo->teamSpotter[teamId] = bestSensor->owner->getHandle();
		}
	}
}

//---------------------------------------------------------------------------
#if 0
void TeamSensorSystem::updateContactList (void) {
	long i;

	numAllContacts = 0;
	for (i = 0; i < ObjectManager->getNumMovers(); i++) {
		MoverPtr mover = ObjectManager->getMover(i);
		if (mover) {
			long bestStatus = CONTACT_NONE;
			// PORT: same merged-counter defect as warrior.cpp calcTarget -- the
			// original inner loop declared its own long i. Dead code today
			// (#if 0), fixed so re-enabling it cannot clobber the mover index.
			for (long t = 0; t < Team::numTeams; t++) {
				if (Team::teams[teamId]->isFriendly(Team::teams[t]))
					if (mover->contactInfo->contactStatus[t] > bestStatus)
						bestStatus = mover->contactInfo->contactStatus[t];
			}
			mover->contactInfo->allContactStatus[teamId] = bestStatus;
			if (bestStatus != CONTACT_NONE) {
				allContacts[numAllContacts++] = mover->getHandle();
			}
		}
	}
}
#endif
//---------------------------------------------------------------------------

void TeamSensorSystem::updateEcmEffects (void) {
	long i;

	for (i = 0; i < numSensors; i++)
		sensors[i]->ecmEffect = SensorManager->getEcmEffect(sensors[i]->owner); 
}

//***************************************************************************
// SENSOR SYSTEM MANAGER class
//***************************************************************************

void* SensorSystemManager::operator new (size_t mySize) {

	void *result = missionHeap->Malloc(mySize);
	return(result);
}

//---------------------------------------------------------------------------

void SensorSystemManager::operator delete (void* us) {

	missionHeap->Free(us);
}

//---------------------------------------------------------------------------

long SensorSystemManager::init (bool debug) {
	long i;

	if (MAX_SENSORS < 2)
		Fatal(0, " Way too few sensors in Sensor System Manager! ");

	sensorPool = (SensorSystemPtr*)missionHeap->Malloc(MAX_SENSORS * sizeof(SensorSystemPtr));
	gosASSERT(sensorPool!=NULL);

	for (i = 0; i < MAX_SENSORS; i++)
		sensorPool[i] = new SensorSystem;

	//-----------------------------------------------------
	// This assumes we have at least 2 sensors in the pool
	// when initializing the pool...
	sensorPool[0]->id = 0;
	sensorPool[0]->prev = NULL;
	sensorPool[0]->next = sensorPool[1];

	for (i = 1; i < (MAX_SENSORS - 1); i++) {
		sensorPool[i]->id = i;
		sensorPool[i]->prev = sensorPool[i - 1];
		sensorPool[i]->next = sensorPool[i + 1];
	}

	sensorPool[MAX_SENSORS - 1]->id = MAX_SENSORS - 1;
	sensorPool[MAX_SENSORS - 1]->prev = sensorPool[MAX_SENSORS - 2];
	sensorPool[MAX_SENSORS - 1]->next = NULL;

	//------------------------------
	// All start on the free list...
	freeList = sensorPool[0];
	freeSensors = MAX_SENSORS;

	for (i = 0; i < MAX_TEAMS; i++)
		teamSensors[i] = NULL;

	Assert (!debug || (Team::numTeams > 0), 0, " SensorSystemManager.init: 0 teams ");

	for (i = 0; i < Team::numTeams; i++) {
		teamSensors[i] = new TeamSensorSystem;
		teamSensors[i]->teamId = i;
	}

	SensorSystem::numSensors = 0;

	return(NO_ERR);
}

long SensorSystemManager::checkIntegrity (void)
{
	long i;
	//See if every pointer in sensorPool is still OK.
	// SOMETHING is trashing memory here.  Damned if I know what.
	long result = 0;

	for (i=0;i<MAX_SENSORS;i++)
	{
		SensorSystem *sensor = getSensor(i);
		if (sensor->owner)
			result = 1;
		else
			result = 0;
	}

	return result;
}

//---------------------------------------------------------------------------

SensorSystemPtr SensorSystemManager::newSensor (void) {

	if (!freeSensors)
		Fatal(0, " No More Free Sensors ");

	freeSensors--;

	//---------------------------------------
	// Grab the first free sensor in line...
	SensorSystemPtr sensor = freeList;

	//------------------------------------------
	// Cut the new sensor from the free list...
	freeList = freeList->next;
	if (freeList)
		freeList->prev = NULL;

	//----------------------------------------------------
	// New system has no next. Already has no previous...
	sensor->next = NULL;

	return(sensor);
}

//---------------------------------------------------------------------------

void SensorSystemManager::freeSensor (SensorSystemPtr sensor) {

	freeSensors++;

	sensor->prev = NULL;
	sensor->next = freeList;
	freeList->prev = sensor;
	freeList = sensor;
}

//---------------------------------------------------------------------------

void SensorSystemManager::destroy (void) {
	long i;

	if (sensorPool)
	{
		for (i = 0; i < MAX_SENSORS; i++) 
		{
			delete sensorPool[i];
			sensorPool[i] = NULL;
		}
		
		missionHeap->Free(sensorPool);
		sensorPool = NULL;
	}

	freeSensors = 0;
	freeList = NULL;

	for (i = 0; i < Team::numTeams; i++) 
	{
		delete teamSensors[i];
		teamSensors[i] = NULL;
	}

	if (SensorSystem::sortList)
	{
		delete SensorSystem::sortList;
		SensorSystem::sortList = NULL;
	}
}

//---------------------------------------------------------------------------

void SensorSystemManager::addTeamSensor (long teamId, SensorSystemPtr sensor) 
{
	if ((teamId > -1) && (teamId < Team::numTeams))
	{
		teamSensors[teamId]->addSensor(sensor);
		sensor->setMaster(teamSensors[teamId]);
	}
}

//---------------------------------------------------------------------------

void SensorSystemManager::removeTeamSensor (long teamId, SensorSystemPtr sensor) {

	if (teamId == -1)
		return;
	teamSensors[teamId]->removeSensor(sensor);
	sensor->setMaster(NULL);
}

//---------------------------------------------------------------------------

void SensorSystemManager::update (void) 
{
	updateEcmEffects();
	updateSensors();
//	updateTeamContactLists();
}

//---------------------------------------------------------------------------

void SensorSystemManager::updateEcmEffects (void) {
	long i;

	for (i = 0; i < Team::numTeams; i++)
		teamSensors[i]->updateEcmEffects();
}

//---------------------------------------------------------------------------

void SensorSystemManager::updateSensors (void) 
{
	/*
	for (long i = 0; i < Team::numTeams; i++)
		teamSensors[i]->update();
	*/
	
	//Let's try one team per frame update!!
	if (Team::teams[teamToUpdate] == Team::home)
		enemyInLOS = false;		//Resets every scan for HOME team!!!!
		
 	teamSensors[teamToUpdate]->update();
	teamToUpdate++;
	if (teamToUpdate == Team::numTeams)
		teamToUpdate = 0;
}

//---------------------------------------------------------------------------
#if 0
void SensorSystemManager::updateTeamContactLists (void) {
	long i;

	for (i = 0; i < Team::numTeams; i++) {
		teamSensors[i]->updateContactList();
	}
}
#endif
//---------------------------------------------------------------------------

void SensorSystemManager::addEcm (GameObjectPtr owner, float range) {

	if (numEcms == MAX_ECMS)
		Fatal(0, " SensorSystemManager.addEcm: too many ecms ");

	ecms[numEcms].owner = owner;
	ecms[numEcms].range = range;
	numEcms++;
}

//---------------------------------------------------------------------------

float SensorSystemManager::getEcmEffect (GameObjectPtr victim) {

	/*		ECM stealths the mech carrying it.  Period.
	for (long i = 0; i < numEcms; i++)
		if (!victim->isFriendly(ecms[i].owner))
			if (ecms[i].owner->distanceFrom(victim->getPosition()) <= ecms[i].range)
				return(0.5);
	*/
	return(1.0);
}

//***************************************************************************




