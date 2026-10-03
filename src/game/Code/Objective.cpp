#define OBJECTIVE_CPP
/*************************************************************************************************\
Objective.cpp			: Implementation of the Objective component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include "Terrain.h" // PORT: match case-sensitive host path.

#include "Objective.h"

#include "EString.h"
#include "ECharString.h"

#ifndef INIFILE_H
#include "Inifile.h" // PORT: match case-sensitive host path.
#endif

#ifndef MISSION_H
#include "mission.h"
#endif

#ifndef OBJMGR_H
#include "objmgr.h"
#endif

#ifndef BLDNG_H
#include "bldng.h"
#endif

#ifndef GAMESOUND_H
#include "gamesound.h"
#endif

#ifndef TACMAP_H
#include "TacMap.h"
#endif

#ifndef AFONT_H
#include "AFONT.H" // PORT: match case-sensitive host path.
#endif

#include "LogisticsData.h" // PORT: match case-sensitive host path.
#include "Team.h" // PORT: match case-sensitive host path.
#include "comndr.h"

#include "ControlGui.h"

#include "../resource.h" // PORT: use host-independent include separators.

#include <tchar.h>

#include <assert.h>

float CObjective::s_blinkLength = .5;
float CObjective::s_lastBlinkTime = 0.f;
unsigned long CObjective::s_blinkColor = SB_YELLOW;
aFont* CObjective::s_markerFont = 0;
float MaxExtractUnitDistance = 0.0f;

static bool MoverIsDeadOrDisabled(const long pMoverWID) 
{
	bool retval = false;

	Mover *pMover = (MoverPtr)ObjectManager->getByWatchID(pMoverWID);
	assert(pMover);

	if (pMover->isDisabled() || pMover->isDestroyed()) {
		retval = true;
	} else {
		assert(pMover->getPilot());
		long wstatus = pMover->getPilot()->getStatus();
		if ((WARRIOR_STATUS_DEAD == wstatus)
			|| (WARRIOR_STATUS_EJECTED == wstatus)) {
			retval = true;
		}
	}
	return retval;
}

static bool MoverIsDeadOrDisabled(const MoverPtr pMover) 
{
	bool retval = false;
	assert(pMover);

	if (pMover->isDisabled() || pMover->isDestroyed()) {
		retval = true;
	} else {
		assert(pMover->getPilot());
		long wstatus = pMover->getPilot()->getStatus();
		if ((WARRIOR_STATUS_DEAD == wstatus)
			|| (WARRIOR_STATUS_EJECTED == wstatus)) {
			retval = true;
		}
	}
	return retval;
}

static bool MoverIsDeadOrDisabledOrFled(const MoverPtr pMover) {
	assert(pMover);
	bool retval = false;
	if (pMover->isDisabled() || pMover->isDestroyed()) {
		retval = true;
	} else {
		assert(pMover->getPilot());
		long wstatus = pMover->getPilot()->getStatus();
		if ((WARRIOR_STATUS_DEAD == wstatus)
			|| (WARRIOR_STATUS_EJECTED == wstatus)) {
			retval = true;
		} else if (WARRIOR_STATUS_WITHDRAWN == wstatus) {
			retval = true;
		}
	}
	return retval;
}

static bool MoverIsCaptured(const MoverPtr pMover, const long teamID) {
	assert(pMover);
	bool retval = false;
	if (pMover->isDestroyed()) {
		retval = false;
	} else {
		if ( pMover->getTeamId() == teamID ) {
			retval = true;
		}
	}
	return retval;
}

static bool MoverIsCaptured(const long pMoverWID, const long teamID) 
{
	Mover *pMover = (MoverPtr)ObjectManager->getByWatchID(pMoverWID);
	assert(pMover);

	bool retval = false;
	if (pMover->isDestroyed()) 
	{
		retval = false;
	} 
	else 
	{
		if ( pMover->getTeamId() == teamID ) 
		{
			retval = true;
		}
	}

	return retval;
}

static long sReadIdFloat(FitIniFile* missionFile, const char *varName, float &value) {
	long result = 0;
	float tmpFloat;
	result = missionFile->readIdFloat((char *)varName, tmpFloat);
	if (NO_ERR != result) {
		//assert(false);
	} else {
		value = tmpFloat;
	}
	return result;
}

static long sReadIdBoolean(FitIniFile* missionFile, const char *varName, bool &value) {
	long result = 0;
	bool tmpBool;
	result = missionFile->readIdBoolean((char *)varName, tmpBool);
	if (NO_ERR != result) {
		//assert(false);
	} else {
		value = tmpBool;
	}
	return result;
}

static long sReadIdWholeNum(FitIniFile* missionFile, const char *varName, int &value) {
	long result = 0;
	unsigned long tmpULong;
	result = missionFile->readIdULong((char *)varName, tmpULong);
	if (NO_ERR != result) {
		//assert(false);
	} else {
		value = tmpULong;
	}
	return result;
}

static long sReadIdLongInt(FitIniFile* missionFile, const char *varName, int &value) {
	long result = 0;
	long tmpLong;
	result = missionFile->readIdLong((char *)varName, tmpLong);
	if (NO_ERR != result) {
		//assert(false);
	} else {
		value = tmpLong;
	}
	return result;
}

static int sReplace(ECharString &ECStr, const char *szOldSub, const char *szNewSub) {
	if ((!szOldSub) || (0 >= strlen(szOldSub)) || (!szNewSub)) { return -1; }
	int retval = 0;
	ECharString newStr;
	const int lengthOfOldSub = strlen(szOldSub);
	int endIndexOfLastOldSub = -1;
	int startIndexOfOldSub = ECStr.Find(szOldSub, endIndexOfLastOldSub + 1);
	while ((ECharString::INVALID_INDEX != startIndexOfOldSub) && (0 <= startIndexOfOldSub) && (ECStr.Length() - lengthOfOldSub >= startIndexOfOldSub)) {
		if (1 <= startIndexOfOldSub) {
			newStr += ECStr.SubString(endIndexOfLastOldSub + 1, startIndexOfOldSub - 1);
		}
		newStr += szNewSub;
		retval += 1;
		endIndexOfLastOldSub = startIndexOfOldSub + lengthOfOldSub -1;
		startIndexOfOldSub = ECStr.Find(szOldSub, endIndexOfLastOldSub + 1);
	}
	if (0 < retval) {
		if (endIndexOfLastOldSub + 1 < ECStr.Length() - 1) {
			newStr += ECStr.SubString(endIndexOfLastOldSub + 1, ECStr.Length() - 1);
		}
		ECStr = newStr;
	}
	return retval;
}

static long sReadIdString(FitIniFile* missionFile, const char *varName, ECharString &ECStr) {
	long result = 0;
	char buffer[2001/*buffer size*/]; buffer[0] = '\0';
	result = missionFile->readIdString((char *)varName, buffer, 2001/*buffer size*/ -1);
	if ((NO_ERR != result) && (BUFFER_TOO_SMALL != result)) {
		//assert(false);
	} else {
		ECStr = buffer;
		/*readIdString can't read in "\r\n"*/
		sReplace(ECStr, "\n", "\r\n");
	}
	return result;
}

static long sWriteIdString(FitIniFile* missionFile, const char *varName, const char *szStr) {
	if (!szStr) { return !(NO_ERR); }
	long result = 0;
	ECharString ECStr = szStr;
	/*readIdString can't read in "\r\n"*/
	sReplace(ECStr, "\r\n", "\n");
	result = missionFile->writeIdString(varName, ECStr.Data());
	return result;
}

static bool areCloseEnough(float f1, float f2)
{
	bool retval = false;
	float diff = fabs(f1 - f2);
	float bigger = fabs(f1);
	if (fabs(f2) > bigger) {
		bigger = fabs(f2);
	}
	float proportion_diff = 0.0;
	if (0.0 != bigger) {
		proportion_diff = diff / bigger;
	}
	if ((pow(10.0, -FLT_DIG) * 10.0/*arbitrary*/) > proportion_diff) {
		retval = true;
	}
	return retval;
}

void CBooleanArray::save (long alignment, FitIniFile *file)
{
	char blockID[256];
	sprintf(blockID, "Team%dBooleanFlags",alignment);
	long result = file->writeBlock(blockID);

	CEStringList::EIterator flagIDListIter;
	long count = 0;
	for (flagIDListIter = m_FlagIDList.Begin(); !flagIDListIter.IsDone(); flagIDListIter++) 
	{
		char stringName[1024];
		sprintf(stringName,"BooleanString%d",count);
		sWriteIdString(file,stringName,(*flagIDListIter).Data());

		char booleanName[1024];
		sprintf(booleanName,"BooleanValue%d",count);
		file->writeIdBoolean(booleanName,m_valueList[count]);

		count++;
	}
}

void CBooleanArray::load (long alignment, FitIniFile *file)
{
	char blockID[256];
	sprintf(blockID, "Team%dBooleanFlags",alignment);
	long result = file->seekBlock(blockID);
	if (result == NO_ERR)
	{
		ECharString tmpECStr;
		EString element;
		bool value;
	
		long count = 0;
		char stringName[1024];
		sprintf(stringName,"BooleanString%d",count);
		result = sReadIdString(file,stringName,tmpECStr);
		while (result == NO_ERR)
		{
			char booleanName[1024];
			sprintf(booleanName,"BooleanValue%d",count);
			result = file->readIdBoolean(booleanName,value);
	
			element = tmpECStr.Data();
			m_FlagIDList.Append(element);
			m_valueList.Append(value);
	
			count++;
			sprintf(stringName,"BooleanString%d",count);
			result = sReadIdString(file,stringName,tmpECStr);
		}
	}
}


objective_status_type CDestroyAllEnemyUnits::Status() {
	long numMovers = ObjectManager->getNumMovers();
	long i;
	for (i = 0; i < numMovers; i+= 1) {
		MoverPtr pMover = ObjectManager->getMover(i);
		assert(pMover);
		if (pMover->getTeamId() != Alignment())
			if (!(MoverIsDeadOrDisabled(pMover) || MoverIsCaptured(pMover, Alignment()))) {
				return OS_UNDETERMINED;
		}
	}
	return OS_SUCCESSFUL;
}

bool CNumberOfUnitsObjectiveCondition::Read( FitIniFile* missionFile )
{
	long result = 0;
	result = sReadIdWholeNum(missionFile, "Num", m_num);
	if (NO_ERR != result) { return false; }

	return true;
}

EString CNumberOfUnitsObjectiveCondition::InstanceDescription() {
	EString tmpEStr;
	tmpEStr.Format("%d", m_num);
	return tmpEStr;
}

objective_status_type CDestroyNumberOfEnemyUnits::Status() {
	int numDestroyed = 0;
	long numMovers = ObjectManager->getNumMovers();
	long i;
	for (i = 0; i < numMovers; i+= 1) {
		MoverPtr pMover = ObjectManager->getMover(i);
		assert(pMover);
		if (pMover->getTeamId() != Alignment())
			if (MoverIsDeadOrDisabled(pMover) || MoverIsCaptured(pMover, Alignment())) {
				numDestroyed += 1;
			}
	}

	if (numDestroyed >= m_num) {
		return OS_SUCCESSFUL;
	} else {
		return OS_UNDETERMINED;
	}
}

EString CSpecificUnitObjectiveCondition::InstanceDescription() {
	EString tmpEStr;
	long partId = m_pUnitWID;

	tmpEStr.Format("partId: %ld", partId);
	return tmpEStr;
}

bool CSpecificEnemyUnitObjectiveCondition::Read( FitIniFile* missionFile )
{
	long result = 0;

	result = missionFile->readIdLong("MoverWID", m_pUnitWID);
	if (result != NO_ERR)
	{
		float positionX, positionY;
		result = sReadIdFloat(missionFile, "PositionX", positionX);
		if (NO_ERR != result) { return false; }
		result = sReadIdFloat(missionFile, "PositionY", positionY);
		if (NO_ERR != result) { return false; }
		MoverPtr pMover = NULL;
		if (ObjectManager) {
			long numMovers = ObjectManager->getNumMovers();
			long i;
			for (i = 0; i < numMovers; i+= 1) {
				pMover = ObjectManager->getMover(i);
				if (pMover && (pMover->getTeamId() != Alignment()) && areCloseEnough(pMover->getPosition().x, positionX)  && areCloseEnough(pMover->getPosition().y, positionY)) {
					break;
				}
			}
			if (!(i < numMovers)) { assert(false); return false; }
		}
		if (pMover)
			m_pUnitWID = pMover->getWatchID();
		else
			m_pUnitWID = 0;
	}

	return true;
}

objective_status_type CDestroySpecificEnemyUnit::Status() {
	assert(m_pUnitWID);
	if (MoverIsDeadOrDisabled(m_pUnitWID) || MoverIsCaptured(m_pUnitWID, Alignment())) {
		return OS_SUCCESSFUL;
	} else {
		return OS_UNDETERMINED;
	}
}

EString CSpecificStructureObjectiveCondition::InstanceDescription() {
	EString tmpEStr;
	Building * m_pBuilding = (Building *)ObjectManager->getByWatchID(m_pBuildingWID);
	long partId = m_pBuilding->getPartId();

	tmpEStr.Format("partId: %ld", partId);
	return tmpEStr;
}

bool CSpecificStructureObjectiveCondition::Read( FitIniFile* missionFile )
{
	long result = 0;

	result = missionFile->readIdLong("BuildingWID",m_pBuildingWID);
	if (result != NO_ERR)
	{
		float positionX, positionY;
		result = sReadIdFloat(missionFile, "PositionX", positionX);
		if (NO_ERR != result) { return false; }
		result = sReadIdFloat(missionFile, "PositionY", positionY);
		if (NO_ERR != result) { return false; }
		BuildingPtr pBuilding = 0;
		if (ObjectManager) {
			pBuilding = ObjectManager->getBuilding(0);
			long i;
			for (i = 0; (0 != pBuilding); i+= 1) {
				pBuilding = ObjectManager->getBuilding(i);
				if ((0 != pBuilding) && areCloseEnough(pBuilding->getPosition().x, positionX)  && areCloseEnough(pBuilding->getPosition().y, positionY)) {
					break;
				}
			}
			if (!pBuilding) { assert(false); return false; }
		}
		if (pBuilding)
			m_pBuildingWID = pBuilding->getWatchID();
		else
			m_pBuildingWID = 0;
	}

	return true;
}

objective_status_type CDestroySpecificStructure::Status() 
{
	Building *m_pBuilding = (Building *)ObjectManager->getByWatchID(m_pBuildingWID);
	if (m_pBuilding->isDestroyed()) {
		return OS_SUCCESSFUL;
	} else {
		return OS_UNDETERMINED;
	}
}

objective_status_type CCaptureOrDestroyAllEnemyUnits::Status() {
	long numMovers = ObjectManager->getNumMovers();
	long i;
	for (i = 0; i < numMovers; i+= 1) {
		MoverPtr pMover = ObjectManager->getMover(i);
		assert(pMover);
		if (pMover->getTeamId() != Alignment())
			if (!MoverIsDeadOrDisabled(pMover)) {
				if (!MoverIsCaptured(pMover, Alignment())) { /*Is this necessary? When a mover is captured, is it no longer returned as a "BadMover"? */
					return OS_UNDETERMINED;
				}
		}
	}
	return OS_SUCCESSFUL;
}

objective_status_type CCaptureOrDestroyNumberOfEnemyUnits::Status() {
	int numCapturedOrDestroyed = 0;
	long numMovers = ObjectManager->getNumMovers();
	long i;
	for (i = 0; i < numMovers; i+= 1) {
		MoverPtr pMover = ObjectManager->getMover(i);
		assert(pMover);
		if (pMover->getTeamId() != Alignment())
			if (MoverIsDeadOrDisabled(pMover) || MoverIsCaptured(pMover, Alignment())/*redundant?*/) {
				numCapturedOrDestroyed += 1;
			}
	}

	if (numCapturedOrDestroyed >= m_num) {
		return OS_SUCCESSFUL;
	} else {
		return OS_UNDETERMINED;
	}
}

bool CCaptureOrDestroySpecificEnemyUnit::Read( FitIniFile* missionFile )
{
	long result = 0;

	result = missionFile->readIdLong("MoverWID", m_pUnitWID);
	if (result != NO_ERR)
	{
		float positionX, positionY;
		result = sReadIdFloat(missionFile, "PositionX", positionX);
		if (NO_ERR != result) { return false; }
		result = sReadIdFloat(missionFile, "PositionY", positionY);
		if (NO_ERR != result) { return false; }
		MoverPtr pMover = 0;
		if (ObjectManager) {
			long numMovers = ObjectManager->getNumMovers();
			long i;
			for (i = 0; i < numMovers; i+= 1) {
				pMover = ObjectManager->getMover(i);
				if (pMover && (pMover->getTeamId() != Alignment()) && areCloseEnough(pMover->getPosition().x, positionX)  && areCloseEnough(pMover->getPosition().y, positionY)) {
					break;
				}
			}
			if (!(i < numMovers)) { assert(false); return false; }
			pMover->setFlag(OBJECT_FLAG_CAPTURABLE, true);
		}
		if (pMover)
			m_pUnitWID = pMover->getWatchID();
		else
			m_pUnitWID = 0;
	}

	return true;
}

objective_status_type CCaptureOrDestroySpecificEnemyUnit::Status() 
{
	MoverPtr m_pUnit = (MoverPtr)ObjectManager->getByWatchID(m_pUnitWID);
	assert(m_pUnit);
	if (MoverIsDeadOrDisabled(m_pUnit)|| MoverIsCaptured(m_pUnit, Alignment())/*redundant?*/) {
		return OS_SUCCESSFUL;
	} else {
		return OS_UNDETERMINED;
	}
}

bool CCaptureOrDestroySpecificStructure::Read( FitIniFile* missionFile )
{
	long result = 0;

	result = missionFile->readIdLong("BuildingWID",m_pBuildingWID);
	if (result != NO_ERR)
	{
		float positionX, positionY;
		result = sReadIdFloat(missionFile, "PositionX", positionX);
		if (NO_ERR != result) { return false; }
		result = sReadIdFloat(missionFile, "PositionY", positionY);
		if (NO_ERR != result) { return false; }
		BuildingPtr pBuilding = 0;
		if (ObjectManager) {
			pBuilding = ObjectManager->getBuilding(0);
			long i;
			for (i = 0; (0 != pBuilding); i+= 1) {
				pBuilding = ObjectManager->getBuilding(i);
				if ((0 != pBuilding) && areCloseEnough(pBuilding->getPosition().x, positionX)  && areCloseEnough(pBuilding->getPosition().y, positionY)) {
					break;
				}
			}
			if (!pBuilding) { assert(false); return false; }
			pBuilding->setFlag(OBJECT_FLAG_CAPTURABLE, true);
		}
		if (pBuilding)
			m_pBuildingWID = pBuilding->getWatchID();
		else
			m_pBuildingWID = 0;
	}

	return true;
}

objective_status_type CCaptureOrDestroySpecificStructure::Status() 
{
	Building* m_pBuilding = (Building *)ObjectManager->getByWatchID(m_pBuildingWID);
	if (m_pBuilding->isDestroyed()) 
	{
		return OS_SUCCESSFUL;
	} 
	else if ( m_pBuilding->getTeamId() == Alignment() ) 
	{
		return OS_SUCCESSFUL;
	} 
	else 
	{
		return OS_UNDETERMINED;
	}
}

objective_status_type CDeadOrFledAllEnemyUnits::Status() {
	long numMovers = ObjectManager->getNumMovers();
	long i;
	for (i = 0; i < numMovers; i+= 1) {
		MoverPtr pMover = ObjectManager->getMover(i);
		assert(pMover);
		if (!MoverIsDeadOrDisabledOrFled(pMover)) {
			return OS_UNDETERMINED;
		}
	}
	return OS_SUCCESSFUL;
}

objective_status_type CDeadOrFledNumberOfEnemyUnits::Status() {
	int numDeadOrFled = 0;
	long numMovers = ObjectManager->getNumMovers();
	long i;
	for (i = 0; i < numMovers; i+= 1) {
		MoverPtr pMover = ObjectManager->getMover(i);
		assert(pMover);
		if (MoverIsDeadOrDisabledOrFled(pMover)) {
			numDeadOrFled += 1;
		}
	}

	if (numDeadOrFled >= m_num) {
		return OS_SUCCESSFUL;
	} else {
		return OS_UNDETERMINED;
	}
}

objective_status_type CDeadOrFledSpecificEnemyUnit::Status() 
{
	MoverPtr m_pUnit = (MoverPtr)ObjectManager->getByWatchID(m_pUnitWID);
	assert(m_pUnit);
	if (MoverIsDeadOrDisabledOrFled(m_pUnit)) {
		return OS_SUCCESSFUL;
	} else {
		return OS_UNDETERMINED;
	}
}

bool CCaptureUnit::Read( FitIniFile* missionFile )
{
	long result = 0;

	result = missionFile->readIdLong("MoverWID", m_pUnitWID);
	if (result != NO_ERR)
	{
		float positionX, positionY;
		result = sReadIdFloat(missionFile, "PositionX", positionX);
		if (NO_ERR != result) { return false; }
		result = sReadIdFloat(missionFile, "PositionY", positionY);
		if (NO_ERR != result) { return false; }
		MoverPtr pMover = 0;
		if (ObjectManager) {
			long numMovers = ObjectManager->getNumMovers();
			long i;
			for (i = 0; i < numMovers; i+= 1) {
				pMover = ObjectManager->getMover(i);
				if (pMover && (pMover->getTeamId() != Alignment()) && areCloseEnough(pMover->getPosition().x, positionX)  && areCloseEnough(pMover->getPosition().y, positionY)) {
					break;
				}
			}
			if (!(i < numMovers)) { assert(false); return false; }
			pMover->setFlag(OBJECT_FLAG_CAPTURABLE, true);
		}
		if (pMover)
			m_pUnitWID = pMover->getWatchID();
		else
			m_pUnitWID = 0;
	}

	return true;
}

objective_status_type CCaptureUnit::Status() 
{
	MoverPtr m_pUnit = (MoverPtr)ObjectManager->getByWatchID(m_pUnitWID);

	if (!m_pUnit || m_pUnit->isDestroyed()) 
	{
		return OS_FAILED;
	} 
	else if (m_pUnit && m_pUnit->getTeamId() == Alignment() ) 
	{
		return OS_SUCCESSFUL;
	} 
	else 
	{
		return OS_UNDETERMINED;
	}
}

bool CCaptureStructure::Read( FitIniFile* missionFile )
{
	long result = 0;
	result = missionFile->readIdLong("BuildingWID", m_pBuildingWID);
	if (result != NO_ERR)
	{
		float positionX, positionY;
		result = sReadIdFloat(missionFile, "PositionX", positionX);
		if (NO_ERR != result) { return false; }
		result = sReadIdFloat(missionFile, "PositionY", positionY);
		if (NO_ERR != result) { return false; }
		BuildingPtr pBuilding = 0;
		if (ObjectManager) 
		{
			pBuilding = ObjectManager->getBuilding(0);
			long i;
			for (i = 0; (0 != pBuilding); i+= 1) 
			{
				pBuilding = ObjectManager->getBuilding(i);
				if ((0 != pBuilding) && areCloseEnough(pBuilding->getPosition().x, positionX)  && areCloseEnough(pBuilding->getPosition().y, positionY)) 
				{
					break;
				}
			}
			if (!pBuilding) { assert(false); return false; }
			pBuilding->setFlag(OBJECT_FLAG_CAPTURABLE, true);
		}
		if (pBuilding)
			m_pBuildingWID = pBuilding->getWatchID();
		else
			m_pBuildingWID = 0;
	}

	return true;
}

objective_status_type CCaptureStructure::Status() 
{
	Building *m_pBuilding = (Building *)ObjectManager->getByWatchID(m_pBuildingWID);
	if (m_pBuilding->isDestroyed()) 
	{
		return OS_FAILED;
	} 
	else if ( m_pBuilding->getTeamId() == Alignment() ) 
	{
		return OS_SUCCESSFUL;
	} 
	else 
	{
		return OS_UNDETERMINED;
	}
}

bool CGuardSpecificUnit::Read( FitIniFile* missionFile )
{
	long result = 0;

	result = missionFile->readIdLong("MoverWID",m_pUnitWID);
	if (result != NO_ERR)
	{
		float positionX, positionY;
		result = sReadIdFloat(missionFile, "PositionX", positionX);
		if (NO_ERR != result) { return false; }
		result = sReadIdFloat(missionFile, "PositionY", positionY);
		if (NO_ERR != result) { return false; }
		MoverPtr pMover = 0;
		if (ObjectManager) {
			long numMovers = ObjectManager->getNumMovers();
			long i;
			for (i = 0; i < numMovers; i+= 1) {
				pMover = ObjectManager->getMover(i);
				if (pMover && (pMover->getTeamId() == Alignment()) && areCloseEnough(pMover->getPosition().x, positionX)  && areCloseEnough(pMover->getPosition().y, positionY)) {
					break;
				}
			}
			if (!(i < numMovers)) { assert(false); return false; }
		}
		if (pMover)
			m_pUnitWID = pMover->getWatchID();
		else
			m_pUnitWID = 0;
	}

	return true;
}

objective_status_type CGuardSpecificUnit::Status() 
{
	Mover *m_pUnit = (Mover *)ObjectManager->getByWatchID(m_pUnitWID);

	if (m_pUnit->isDestroyed()) {
		return OS_FAILED;
	} else if (  m_pUnit->getTeamId() != Alignment() ) {
		return OS_FAILED;
	} else {
		return OS_UNDETERMINED;
	}
}

objective_status_type CGuardSpecificStructure::Status() 
{
	Building *m_pBuilding = (Building *)ObjectManager->getByWatchID(m_pBuildingWID);
	if (m_pBuilding->isDestroyed()) 
	{
		return OS_FAILED;
	}
	else if ( m_pBuilding->getCaptured() && (m_pBuilding->getTeamId() != Alignment())) 
	{
		return OS_FAILED;
	} 
	else if ((mission->m_timeLimit != -1.0f) && (mission->actualTime > mission->m_timeLimit)) 
	{
		return OS_SUCCESSFUL;
	} 
	else 
	{
		return OS_UNDETERMINED;
	}
}

bool CAreaObjectiveCondition::Read( FitIniFile* missionFile )
{
	long result = 0;
	result = sReadIdFloat(missionFile, "TargetCenterX", m_targetCenterX);
	if (NO_ERR != result) { return false; }
	result = sReadIdFloat(missionFile, "TargetCenterY", m_targetCenterY);
	if (NO_ERR != result) { return false; }
	result = sReadIdFloat(missionFile, "TargetRadius", m_targetRadius);
	if (NO_ERR != result) { return false; }
	return true;
}

EString CAreaObjectiveCondition::InstanceDescription() {
	EString tmpEStr;
	tmpEStr.Format("x: %.3f, y: %.3f, radius: %.3f", m_targetCenterX, m_targetCenterY, m_targetRadius);
	return tmpEStr;
}

objective_status_type CMoveAnyUnitToArea::Status() 
{
	long numMovers = ObjectManager->getNumMovers();
	long i;
	if (MPlayer) {
		for (i = 0; i < numMovers; i+= 1) 
		{
			MoverPtr pMover = ObjectManager->getMover(i);
			assert(pMover);

			if (pMover->getTeamId() == Alignment())
			{
				float dx = pMover->getPosition().x - m_targetCenterX;
				float dy = pMover->getPosition().y - m_targetCenterY;
				if (dx*dx + dy*dy < m_targetRadius*m_targetRadius) 
				{
					return OS_SUCCESSFUL;
				}
			}
		}
		}
	else {
		for (i = 0; i < numMovers; i+= 1) 
		{
			MoverPtr pMover = ObjectManager->getMover(i);
			assert(pMover);

			if (pMover->isOnGUI() && (pMover->getCommanderId() == Commander::home->getId()))
			{
				float dx = pMover->getPosition().x - m_targetCenterX;
				float dy = pMover->getPosition().y - m_targetCenterY;
				if (dx*dx + dy*dy < m_targetRadius*m_targetRadius) 
				{
					return OS_SUCCESSFUL;
				}
			}
		}
	}
	return OS_UNDETERMINED;
}

objective_status_type CMoveAllUnitsToArea::Status() 
{
	long numMovers = ObjectManager->getNumMovers();
	long i;
	if (MPlayer) {
		for (i = 0; i < numMovers; i+= 1) 
		{
			MoverPtr pMover = ObjectManager->getMover(i);
			assert(pMover);

			if (pMover->isRecover()) 
				continue;

			//Seems like any allied or unlinked up unit would keep this from being true?
			if (pMover->getTeamId() == Alignment())
			{
				float dx = pMover->getPosition().x - m_targetCenterX;
				float dy = pMover->getPosition().y - m_targetCenterY;
				if (dx*dx + dy*dy > m_targetRadius*m_targetRadius) 
				{
					return OS_UNDETERMINED;
				}
			}
		}
		}
	else {
		for (i = 0; i < numMovers; i+= 1) 
		{
			MoverPtr pMover = ObjectManager->getMover(i);
			assert(pMover);

			if (pMover->isRecover()) 
				continue;

			//Seems like any allied or unlinked up unit would keep this from being true?
			if (pMover->isOnGUI() && (pMover->getCommanderId() == Commander::home->getId()))
			{
				float dx = pMover->getPosition().x - m_targetCenterX;
				float dy = pMover->getPosition().y - m_targetCenterY;
				if (dx*dx + dy*dy > m_targetRadius*m_targetRadius) 
				{
					return OS_UNDETERMINED;
				}
			}
		}
	}

	return OS_SUCCESSFUL;
}

objective_status_type CMoveAllSurvivingUnitsToArea::Status() 
{
	long numMovers = ObjectManager->getNumMovers();
	long i;
	if (MPlayer) {
		for (i = 0; i < numMovers; i+= 1) 
		{
			MoverPtr pMover = ObjectManager->getMover(i);
			assert(pMover);
			if (pMover->isRecover()) 
				continue;

			//Seems like any allied or unlinked up unit would keep this from being true?
			if (pMover->getTeamId() == Alignment())
			{
				if (!MoverIsDeadOrDisabledOrFled(pMover)) 
				{
					float dx = pMover->getPosition().x - m_targetCenterX;
					float dy = pMover->getPosition().y - m_targetCenterY;
					if (dx*dx + dy*dy > m_targetRadius*m_targetRadius) 
					{
						return OS_UNDETERMINED;
					}
				}
			}
		}
		}
	else {
		for (i = 0; i < numMovers; i+= 1) 
		{
			MoverPtr pMover = ObjectManager->getMover(i);
			assert(pMover);
			if (pMover->isRecover()) 
				continue;

			//Seems like any allied or unlinked up unit would keep this from being true?
			if (pMover->isOnGUI() && (pMover->getCommanderId() == Commander::home->getId()))
			{
				if (!MoverIsDeadOrDisabledOrFled(pMover)) 
				{
					float dx = pMover->getPosition().x - m_targetCenterX;
					float dy = pMover->getPosition().y - m_targetCenterY;
					if (dx*dx + dy*dy > m_targetRadius*m_targetRadius) 
					{
						return OS_UNDETERMINED;
					}
				}
			}
		}
	}

	return OS_SUCCESSFUL;
}

objective_status_type CMoveAllSurvivingMechsToArea::Status() 
{
	long numMovers = ObjectManager->getNumMovers();
	long i;
	if (MPlayer) 
	{
		for (i = 0; i < numMovers; i+= 1) 
		{
			MoverPtr pMover = ObjectManager->getMover(i);
			assert(pMover);
			if (!pMover->isMech() || (pMover->isMech() && (pMover->getMoveType() == MOVETYPE_AIR))) 
				continue;

			//Seems like any allied or unlinked up unit would keep this from being true?
			if (pMover->getTeamId() == Alignment())
			{
				if (!MoverIsDeadOrDisabledOrFled(pMover)) 
				{
					float dx = pMover->getPosition().x - m_targetCenterX;
					float dy = pMover->getPosition().y - m_targetCenterY;
					if (dx*dx + dy*dy > m_targetRadius*m_targetRadius) 
					{
						return OS_UNDETERMINED;
					}
				}
			}
		}		
	}
	else 
	{
		for (i = 0; i < numMovers; i+= 1) 
		{
			MoverPtr pMover = ObjectManager->getMover(i);
			assert(pMover);
			if (!pMover->isMech() || (pMover->isMech() && (pMover->getMoveType() == MOVETYPE_AIR))) 
				continue;

			//Seems like any allied or unlinked up unit would keep this from being true?
			if (pMover->isOnGUI() && (pMover->getCommanderId() == Commander::home->getId()))
			{
				if (!MoverIsDeadOrDisabledOrFled(pMover)) 
				{
					float dx = pMover->getPosition().x - m_targetCenterX;
					float dy = pMover->getPosition().y - m_targetCenterY;
					if (dx*dx + dy*dy > m_targetRadius*m_targetRadius) 
					{
						return OS_UNDETERMINED;
					}
				}
			}
		}

		//All of our units are in the area.  NOW check to make no ENEMY units are within x Distance of the area.
		float MaxUnitDistanceSquared = (MaxExtractUnitDistance * MaxExtractUnitDistance);
		for (i = 0; i < numMovers; i+= 1) 
		{
			MoverPtr pMover = ObjectManager->getMover(i);
			assert(pMover);

			//Seems like any allied or unlinked up unit would keep this from being true?
			if (pMover->getCommanderId() != Commander::home->getId())
			{
				if (!MoverIsDeadOrDisabledOrFled(pMover)) 
				{
					float dx = pMover->getPosition().x - m_targetCenterX;
					float dy = pMover->getPosition().y - m_targetCenterY;
					if (dx*dx + dy*dy < MaxUnitDistanceSquared)
					{
						return OS_UNDETERMINED;
					}
				}
			}
		}
	}

	return OS_SUCCESSFUL;
}

bool CBooleanFlagIsSet::Read( FitIniFile* missionFile )
{
	long result = 0;
	ECharString tmpECStr;
	result = sReadIdString(missionFile, "FlagID", tmpECStr);
	if (NO_ERR != result) {
		/*for backward compatibility*/
		int flagIndex = 0;
		result = sReadIdWholeNum(missionFile, "FlagIndex", flagIndex);
		if (NO_ERR != result) { return false; }
		m_flagID.Format("%d", flagIndex);
	} else {
		m_flagID = tmpECStr.Data();
	}
	result = sReadIdBoolean(missionFile, "Value", m_value);
	return true;
}

objective_status_type CBooleanFlagIsSet::Status() 
{
	//if (mission->missionObjectives.boolFlags.getElementValue(m_flagID) == m_value) {
	if (Team::teams[Alignment()]->objectives.boolFlags.getElementValue(m_flagID) == m_value) {
		return OS_SUCCESSFUL;
	}
	return OS_UNDETERMINED;
}

EString CBooleanFlagIsSet::InstanceDescription() {
	EString tmpEStr;
	tmpEStr.Format("flag name: %s, value: %d", m_flagID.Data(), m_value);
	return tmpEStr;
}

bool CElapsedMissionTime::Read( FitIniFile* missionFile )
{
	long result = 0;
	result = sReadIdFloat(missionFile, "Time", m_time);
	if (NO_ERR != result) { return false; }
	return true;
}

objective_status_type CElapsedMissionTime::Status() {
	if (mission->actualTime >= m_time) {
		return OS_SUCCESSFUL;
	}
	return OS_UNDETERMINED;
}

EString CElapsedMissionTime::InstanceDescription() {
	EString tmpEStr;
	tmpEStr.Format("%f seconds", m_time);
	return tmpEStr;
}

bool CPlayBIK::Read( FitIniFile* missionFile )
{
	long result = 0;
	ECharString tmpECStr;
	result = sReadIdString(missionFile, "AVIFileName", tmpECStr);
	if (NO_ERR != result) { return false; }

	ECharString tmpECStr2 = "./";
	tmpECStr2 += tmpECStr;
	m_pathname = tmpECStr2.Data();

	return true;
}

EString CPlayBIK::InstanceDescription() {
	EString tmpEStr;
	tmpEStr.Format("%s", m_pathname.Data());
	return tmpEStr;
}

int CPlayBIK::Execute()
{
	ControlGui::instance->playMovie( m_pathname );
	return true;
}

bool CPlayWAV::Read( FitIniFile* missionFile )
{
	long result = 0;
	ECharString tmpECStr;
	result = sReadIdString(missionFile, "WAVFileName", tmpECStr);
	if (NO_ERR != result) { return false; }

	ECharString tmpECStr2 = "./";
	tmpECStr2 += tmpECStr;
	m_pathname = tmpECStr2.Data();

	return true;
}

EString CPlayWAV::InstanceDescription() {
	EString tmpEStr;
	tmpEStr.Format("%s", m_pathname.Data());
	return tmpEStr;
}

int CPlayWAV::Execute()
{
	soundSystem->playDigitalStream(m_pathname.Data());
	return true;
}

bool CDisplayTextMessage::Read( FitIniFile* missionFile )
{
	long result = 0;
	ECharString tmpECStr;
	result = sReadIdString(missionFile, "Message", tmpECStr);
	if (NO_ERR != result) { return false; }
	m_message = tmpECStr.Data();
	return true;
}

EString CDisplayTextMessage::InstanceDescription() {
	EString tmpEStr;
	tmpEStr.Format("%s", m_message.Data());
	return tmpEStr;
}

int CDisplayTextMessage::Execute()
{
	ControlGui::instance->setChatText("", m_message.Data(), 0xf0000000, 0);
	return true;
}

bool CDisplayResourceTextMessage::Read( FitIniFile* missionFile )
{
	long result = 0;
	result = sReadIdWholeNum(missionFile, "ResourceStringID", m_resourceStringID);
	if (NO_ERR != result) { return false; }
	return true;
}

static bool ESLoadString(int resourceID, EString &targetStr) {
	char szTmp[16384/*max string length*/];
	cLoadString( resourceID, szTmp, 16384/*max string length*/ );
	targetStr = szTmp;
	EString tmpStr;
	tmpStr.Format("mc2res.dll:%d Not defined", resourceID);
	if (0 == strcmp(tmpStr.Data(), szTmp)) {
		return (0);
	}
	return (!0);
}

EString CDisplayResourceTextMessage::InstanceDescription() {
	EString resourceText;
	ESLoadString(m_resourceStringID, resourceText);
	EString tmpEStr;
	tmpEStr.Format("%d: %s", m_resourceStringID, resourceText.Data());
	return tmpEStr;
}

int CDisplayResourceTextMessage::Execute()
{
	EString resourceText;
	ESLoadString(m_resourceStringID, resourceText);
	ControlGui::instance->setChatText("", resourceText.Data(), 0xf0000000, 0);
	return true;
}

bool CSetBooleanFlag::Read( FitIniFile* missionFile )
{
	long result = 0;
	ECharString tmpECStr;
	result = sReadIdString(missionFile, "FlagID", tmpECStr);
	if (NO_ERR != result) {
		/*for backward compatibility*/
		int flagIndex = 0;
		result = sReadIdWholeNum(missionFile, "FlagIndex", flagIndex);
		if (NO_ERR != result) { return false; }
		m_flagID.Format("%d", flagIndex);
	} else {
		m_flagID = tmpECStr.Data();
	}
	result = sReadIdBoolean(missionFile, "Value", m_value);
	return true;
}

EString CSetBooleanFlag::InstanceDescription() {
	EString tmpEStr;
	tmpEStr.Format("flag name: %s, value: %d", m_flagID.Data(), m_value);
	return tmpEStr;
}

int CSetBooleanFlag::Execute()
{
	//mission->missionObjectives.boolFlags.setElementValue(m_flagID, m_value);
	Team::teams[Alignment()]->objectives.boolFlags.setElementValue(m_flagID, m_value);
	return true;
}

bool CMakeNewTechnologyAvailable::Read( FitIniFile* missionFile )
{
	long result = 0;
	ECharString tmpECStr;
	result = sReadIdString(missionFile, "PurchaseFileName", tmpECStr);
	if (NO_ERR != result) { return false; }
	m_purchaseFilePathname = tmpECStr.Data();
	return true;
}

EString CMakeNewTechnologyAvailable::InstanceDescription() {
	EString tmpEStr;
	tmpEStr.Format("%s", m_purchaseFilePathname.Data());
	return tmpEStr;
}

int CMakeNewTechnologyAvailable::Execute()
{
	LogisticsData::instance->addNewBonusPurchaseFile( m_purchaseFilePathname );
	return true;
}

bool C_RemoveStructure::SetParams(float positionX, float positionY) {

	BuildingPtr pBuilding = 0;
	if (ObjectManager) 
	{
		pBuilding = ObjectManager->getBuilding(0);
		long i;
		for (i = 0; (0 != pBuilding); i+= 1) {
			pBuilding = ObjectManager->getBuilding(i);
			if ((0 != pBuilding) && (pBuilding->getPosition().x == positionX) && (pBuilding->getPosition().y == positionY)) {
				break;
			}
		}
		if (!pBuilding) { assert(false); return false; }
	}
	if (pBuilding)
		m_pBuildingWID = pBuilding->getWatchID();
	else
		m_pBuildingWID = 0;

	return true;
}

bool C_RemoveStructure::Read( FitIniFile* missionFile )
{
	long result = 0;

	result = missionFile->readIdLong("BuildingWID",m_pBuildingWID);
	if (result != NO_ERR)
	{
		float positionX, positionY;
		result = sReadIdFloat(missionFile, "PositionX", positionX);
		if (NO_ERR != result) { return false; }
		result = sReadIdFloat(missionFile, "PositionY", positionY);
		if (NO_ERR != result) { return false; }
		BuildingPtr pBuilding = 0;
		if (ObjectManager) {
			pBuilding = ObjectManager->getBuilding(0);
			long i;
			for (i = 0; (0 != pBuilding); i+= 1) {
				pBuilding = ObjectManager->getBuilding(i);
				if ((0 != pBuilding) && (pBuilding->getPosition().x == positionX) && (pBuilding->getPosition().y == positionY)) {
					break;
				}
			}
			if (!pBuilding) { assert(false); return false; }
		}
		if (pBuilding)
			m_pBuildingWID = pBuilding->getWatchID();
		else
			m_pBuildingWID = 0;
	}

	return true;
}

EString C_RemoveStructure::InstanceDescription() {
	EString tmpEStr;
	Building * m_pBuilding = (Building *)ObjectManager->getByWatchID(m_pBuildingWID);
	long partId = m_pBuilding->getPartId();

	tmpEStr.Format("partId: %ld", partId);
	return tmpEStr;
}

CObjectiveCondition *CObjective::new_CObjectiveCondition(condition_species_type conditionSpecies, int alignment) {
	CObjectiveCondition *retval = 0;
	switch (conditionSpecies) {
	case DESTROY_ALL_ENEMY_UNITS: retval = new CDestroyAllEnemyUnits(alignment); break;
	case DESTROY_NUMBER_OF_ENEMY_UNITS: retval = new CDestroyNumberOfEnemyUnits(alignment); break;
	case DESTROY_SPECIFIC_ENEMY_UNIT: retval = new CDestroySpecificEnemyUnit(alignment); break;
	case DESTROY_SPECIFIC_STRUCTURE: retval = new CDestroySpecificStructure(alignment); break;
	case CAPTURE_OR_DESTROY_ALL_ENEMY_UNITS: retval = new CCaptureOrDestroyAllEnemyUnits(alignment); break;
	case CAPTURE_OR_DESTROY_NUMBER_OF_ENEMY_UNITS: retval = new CCaptureOrDestroyNumberOfEnemyUnits(alignment); break;
	case CAPTURE_OR_DESTROY_SPECIFIC_ENEMY_UNIT: retval = new CCaptureOrDestroySpecificEnemyUnit(alignment); break;
	case CAPTURE_OR_DESTROY_SPECIFIC_STRUCTURE: retval = new CCaptureOrDestroySpecificStructure(alignment); break;
	case DEAD_OR_FLED_ALL_ENEMY_UNITS: retval = new CDeadOrFledAllEnemyUnits(alignment); break;
	case DEAD_OR_FLED_NUMBER_OF_ENEMY_UNITS: retval = new CDeadOrFledNumberOfEnemyUnits(alignment); break;
	case DEAD_OR_FLED_SPECIFIC_ENEMY_UNIT: retval = new CDeadOrFledSpecificEnemyUnit(alignment); break;
	case CAPTURE_UNIT: retval = new CCaptureUnit(alignment); break;
	case CAPTURE_STRUCTURE: retval = new CCaptureStructure(alignment); break;
	case GUARD_SPECIFIC_UNIT: retval = new CGuardSpecificUnit(alignment); break;
	case GUARD_SPECIFIC_STRUCTURE: retval = new CGuardSpecificStructure(alignment); break;
	case MOVE_ANY_UNIT_TO_AREA: retval = new CMoveAnyUnitToArea(alignment); break;
	case MOVE_ALL_UNITS_TO_AREA: retval = new CMoveAllUnitsToArea(alignment); break;
	case MOVE_ALL_SURVIVING_UNITS_TO_AREA: retval = new CMoveAllSurvivingUnitsToArea(alignment); break;
	case MOVE_ALL_SURVIVING_MECHS_TO_AREA: retval = new CMoveAllSurvivingMechsToArea(alignment); break;
	case BOOLEAN_FLAG_IS_SET: retval = new CBooleanFlagIsSet(alignment); break;
	case ELAPSED_MISSION_TIME: retval = new CElapsedMissionTime(alignment); break;
	default:
		assert(false);
		retval = 0;
		break;
	}
	return retval;
}

EString CObjective::DescriptionOfConditionSpecies(condition_species_type conditionSpecies) {
	EString retval = "<unimplemented condition>" /* needs to be put somewhere localizable */;
	CObjectiveCondition *tmpCondition = new_CObjectiveCondition(conditionSpecies, 0/*dummy alignment*/);
	if (0 != tmpCondition) {
		retval = tmpCondition->Description();
	}
	delete tmpCondition;
	return retval;
}

CObjectiveAction *CObjective::new_CObjectiveAction(action_species_type actionSpecies, int alignment) {
	CObjectiveAction *retval = 0;
	switch (actionSpecies) {
	case PLAY_BIK: retval = new CPlayBIK(alignment); break;
	case PLAY_WAV: retval = new CPlayWAV(alignment); break;
	case DISPLAY_TEXT_MESSAGE: retval = new CDisplayTextMessage(alignment); break;
	case DISPLAY_RESOURCE_TEXT_MESSAGE: retval = new CDisplayResourceTextMessage(alignment); break;
	case SET_BOOLEAN_FLAG: retval = new CSetBooleanFlag(alignment); break;
	case MAKE_NEW_TECHNOLOGY_AVAILABLE: retval = new CMakeNewTechnologyAvailable(alignment); break;

	//ONLY used for in-mission saveLoad to avoid calling ReadNavMarkers
	case _REMOVE_STRUCTURE: retval = new C_RemoveStructure(alignment); break;
	default:
		assert(false);
		retval = 0;
		break;
	}
	return retval;
}

EString CObjective::DescriptionOfActionSpecies(action_species_type actionSpecies) {
	EString retval = "<unimplemented action>" /* needs to be put somewhere localizable */;
	CObjectiveAction *tmpAction = new_CObjectiveAction(actionSpecies, 0/*dummy alignment*/);
	if (0 != tmpAction) {
		retval = tmpAction->Description();
	}
	delete tmpAction;
	return retval;
}

CObjective::CObjective(int alignment) {
	m_alignment = alignment;
	Title(_TEXT(""));
	m_titleUseResourceString = false;
	m_titleResourceStringID = 0;
	Description(_TEXT(""));
	m_descriptionUseResourceString = false;
	m_descriptionResourceStringID = 0;
	m_priority = 1;
	m_resourcePoints = 0;
	m_previousPrimaryObjectiveMustBeComplete = false;
	m_allPreviousPrimaryObjectivesMustBeComplete = false;
	m_displayMarker = false;
	m_modelID = 0;
	m_markerX = 0.0;
	m_markerY = 0.0;
	m_isHiddenTrigger = false;
	m_isActive = true;
	m_activateOnFlag = false;
	ActivateFlagID(_TEXT(""));
	m_activationTime = -1.0;
	m_resetStatusOnFlag = false;
	ResetStatusFlagID(_TEXT(""));
	m_resolved = false;
	m_changedStatus = false;
	m_resolvedStatus = OS_UNDETERMINED;
	m_modelScale = 1.0f;
}

CObjective &CObjective::operator=(const CObjective &master) {
	Clear();
	m_alignment = master.m_alignment;
	Title(master.Title());
	m_titleUseResourceString = master.m_titleUseResourceString;
	m_titleResourceStringID = master.m_titleResourceStringID;
	Description(master.Description());
	m_descriptionUseResourceString = master.m_descriptionUseResourceString;
	m_descriptionResourceStringID = master.m_descriptionResourceStringID;
	m_priority = master.m_priority;
	m_resourcePoints = master.m_resourcePoints;
	m_previousPrimaryObjectiveMustBeComplete = master.m_previousPrimaryObjectiveMustBeComplete;
	m_allPreviousPrimaryObjectivesMustBeComplete = master.m_allPreviousPrimaryObjectivesMustBeComplete;
	m_displayMarker = master.m_displayMarker;
	m_modelID = master.m_modelID;
	m_markerX = master.m_markerX;
	m_markerY = master.m_markerY;
	m_isHiddenTrigger = master.m_isHiddenTrigger;
	m_isActive = master.m_isActive;
	m_activateOnFlag = master.m_activateOnFlag;
	m_activateFlagID = master.m_activateFlagID;
	m_activationTime = master.m_activationTime;
	m_resetStatusOnFlag = master.m_resetStatusOnFlag;
	m_resetStatusFlagID = master.m_resetStatusFlagID;
	m_modelName = master.m_modelName;
	m_modelType = master.m_modelType;
	m_modelBaseColor = master.m_modelBaseColor;
	m_modelHighlightColor = master.m_modelHighlightColor;
	m_modelHighlightColor2 = master.m_modelHighlightColor2;
	m_modelScale = master.m_modelScale;
	m_resolved = master.m_resolved;
	m_changedStatus = master.m_changedStatus;
	if (m_resolved) {
		m_resolvedStatus = master.m_resolvedStatus;
	}

	{
		CObjective::EConstIterator it = master.Begin();
		while (!it.IsDone()) {
			condition_species_type conditionSpecies = (*it)->Species();
			CObjectiveCondition *pTmpCondition = new_CObjectiveCondition(conditionSpecies, m_alignment);
			(*pTmpCondition).CastAndCopy(*it);
			Append(pTmpCondition);
			it++;
		}
	}

	{
		action_list_type::EConstIterator it = master.m_actionList.Begin();
		while (!it.IsDone()) {
			action_species_type actionSpecies = (*it)->Species();
			CObjectiveAction *pTmpAction = new_CObjectiveAction(actionSpecies, m_alignment);
			(*pTmpAction).CastAndCopy(*it);
			m_actionList.Append(pTmpAction);
			it++;
		}
	}

	{
		condition_list_type::EConstIterator it = master.m_failureConditionList.Begin();
		while (!it.IsDone()) {
			condition_species_type conditionSpecies = (*it)->Species();
			CObjectiveCondition *pTmpCondition = new_CObjectiveCondition(conditionSpecies, m_alignment);
			(*pTmpCondition).CastAndCopy(*it);
			m_failureConditionList.Append(pTmpCondition);
			it++;
		}
	}

	{
		action_list_type::EConstIterator it = master.m_failureActionList.Begin();
		while (!it.IsDone()) {
			action_species_type actionSpecies = (*it)->Species();
			CObjectiveAction *pTmpAction = new_CObjectiveAction(actionSpecies, m_alignment);
			(*pTmpAction).CastAndCopy(*it);
			m_failureActionList.Append(pTmpAction);
			it++;
		}
	}

	return (*this);
}

void CObjective::Clear() {
	m_resolved = false;
	m_changedStatus = false;

	{
		EIterator it = Begin();
		while (!it.IsDone()) {
			delete (*it);
			it++;
		}
		inherited::Clear();
	}

	{
		action_list_type::EIterator it = m_actionList.Begin();
		while (!it.IsDone()) {
			delete (*it);
			it++;
		}
		m_actionList.Clear();
	}

	{
		condition_list_type::EIterator it = m_failureConditionList.Begin();
		while (!it.IsDone()) {
			delete (*it);
			it++;
		}
		m_failureConditionList.Clear();
	}

	{
		action_list_type::EIterator it = m_failureActionList.Begin();
		while (!it.IsDone()) {
			delete (*it);
			it++;
		}
		m_failureActionList.Clear();
	}
}

void CObjective::Alignment(int alignment) {
	m_alignment = alignment;
	{
		EIterator it = Begin();
		while (!it.IsDone()) {
			(*it)->Alignment(m_alignment);
			it++;
		}
	}

	{
		action_list_type::EIterator it = m_actionList.Begin();
		while (!it.IsDone()) {
			(*it)->Alignment(m_alignment);
			it++;
		}
	}

	{
		condition_list_type::EIterator it = m_failureConditionList.Begin();
		while (!it.IsDone()) {
			(*it)->Alignment(m_alignment);
			it++;
		}
	}

	{
		action_list_type::EIterator it = m_failureActionList.Begin();
		while (!it.IsDone()) {
			(*it)->Alignment(m_alignment);
			it++;
		}
	}
}

static condition_species_type ConditionSpeciesMap(const char *speciesString) {
	condition_species_type retval = DESTROY_ALL_ENEMY_UNITS;
	int i;
	for (i = 0; i < (int)NUM_CONDITION_SPECIES; i += 1) {
		if (0 == strcmp(speciesString, g_conditionSpeciesStringArray[i])) {
			retval = (condition_species_type)i;
			break;
		}
	}
	assert(i < (int)NUM_CONDITION_SPECIES);
	return retval;
}

static action_species_type ActionSpeciesMap(const char *speciesString) {
	action_species_type retval = PLAY_BIK;
	int i;
	for (i = 0; i < (int)NUM_ACTION_SPECIES; i += 1) {
		if (0 == strcmp(speciesString, g_actionSpeciesStringArray[i])) {
			retval = (action_species_type)i;
			break;
		}
	}
	assert(i < (int)NUM_ACTION_SPECIES);
	return retval;
}

bool CObjective::Read( FitIniFile* missionFile, int objectiveNum, int version, int MarkerNum, char secondary )
{
	//Load this up here to make assert at program start go away!

	if ( !s_markerFont )
	{
		s_markerFont = new aFont;
		// need to use this one arialBlack9.d3f
		s_markerFont->init( IDS_OBJECTIVE_MARKER_FONT );
	}

	long result = 0;
	ECharString tmpECStr;
	result = sReadIdString(missionFile, "Title", tmpECStr);
	Title(_T(tmpECStr.Data()));
	result = sReadIdBoolean(missionFile, "TitleUseResourceString", m_titleUseResourceString);
	result = sReadIdWholeNum(missionFile, "TitleResourceStringID", m_titleResourceStringID);
	result = sReadIdString(missionFile, "Description", tmpECStr);
	Description(_T(tmpECStr.Data()));
	result = sReadIdBoolean(missionFile, "DescriptionUseResourceString", m_descriptionUseResourceString);
	result = sReadIdWholeNum(missionFile, "DescriptionResourceStringID", m_descriptionResourceStringID);

	result = sReadIdWholeNum(missionFile, "Priority", m_priority);
	result = sReadIdWholeNum(missionFile, "ResourcePoints", m_resourcePoints);
	result = sReadIdBoolean(missionFile, "PreviousPrimaryObjectiveMustBeComplete", m_previousPrimaryObjectiveMustBeComplete);
	if (NO_ERR != result) {
		result = sReadIdBoolean(missionFile, "PreviousObjectiveMustBeComplete", m_previousPrimaryObjectiveMustBeComplete);
	}
	result = sReadIdBoolean(missionFile, "AllPreviousPrimaryObjectivesMustBeComplete", m_allPreviousPrimaryObjectivesMustBeComplete);

	m_markerText[0] = 0;

	bool tmp;
	result = sReadIdBoolean(missionFile, "DisplayMarker", tmp);
	if ( tmp )
	{
		m_displayMarker = NUMERIC;
		if ( m_priority == 1)
			sprintf( m_markerText, "%ld", MarkerNum );
		else
			sprintf( m_markerText, "%c", secondary );
	}
	result = sReadIdFloat(missionFile, "MarkerX", m_markerX);
	result = sReadIdFloat(missionFile, "MarkerY", m_markerY);
	result = sReadIdBoolean(missionFile, "HiddenTrigger", m_isHiddenTrigger);
	result = sReadIdBoolean(missionFile, "ActivateOnFlag", m_activateOnFlag);
	if (m_activateOnFlag) { m_isActive = false; }
	result = sReadIdString(missionFile, "ActivateFlagID", tmpECStr);
	if (NO_ERR != result) {
		/*for backward compatibility*/
		int flagIndex = 0;
		result = sReadIdWholeNum(missionFile, "ActivateFlagIndex", flagIndex);
		if (NO_ERR == result) {
			m_activateFlagID.Format("%d", flagIndex);
		}
	} else {
		m_activateFlagID = tmpECStr.Data();
	}
	result = sReadIdBoolean(missionFile, "ResetStatusOnFlag", m_resetStatusOnFlag);
	result = sReadIdString(missionFile, "ResetStatusFlagID", tmpECStr);
	if (NO_ERR == result) {
		m_resetStatusFlagID = tmpECStr.Data();
	}

	result = sReadIdString(missionFile, "ObjectiveModel", tmpECStr);
	ModelName(_T(tmpECStr.Data()));
	result = sReadIdLongInt(missionFile, "ModelType", m_modelType);
	{
		int tmpModelID = 0;
		result = sReadIdLongInt(missionFile, "ModelID", tmpModelID);
		m_modelID = (NO_ERR == result) ? (long)tmpModelID : 0L;
	}
	result = sReadIdLongInt(missionFile, "BaseColor", m_modelBaseColor);
	result = sReadIdLongInt(missionFile, "HighlightColor", m_modelHighlightColor);
	result = sReadIdLongInt(missionFile, "HighlightColor2", m_modelHighlightColor2);
	result = sReadIdFloat(missionFile, "ModelScale", m_modelScale);

	{
		/*these items should only be present in "in-mission" saved games*/
		/*state data*/
		result = sReadIdBoolean(missionFile, "IsActive", m_isActive);
		float tempFloat;
		result = sReadIdFloat(missionFile, "ActivationTime", tempFloat);
		if (NO_ERR == result) {
			m_activationTime = tempFloat;
		}
		result = sReadIdBoolean(missionFile, "IsResolved", m_resolved);
		int tempInt;
		result = sReadIdLongInt(missionFile, "ResolvedStatus", tempInt);
		if (NO_ERR == result) {
			m_resolvedStatus = (objective_status_type)tempInt;
		}
		result = sReadIdBoolean(missionFile, "ChangedStatus", m_changedStatus);
		/*end state data*/
	}

	int numConditions = 0;
	result = sReadIdWholeNum(missionFile, "NumConditions", numConditions);
	if (NO_ERR != result) { return false; }
	int numActions = 0;
	result = sReadIdWholeNum(missionFile, "NumActions", numActions);
	int numFailureConditions = 0;
	result = sReadIdWholeNum(missionFile, "NumFailureConditions", numFailureConditions);
	int numFailureActions = 0;
	result = sReadIdWholeNum(missionFile, "NumFailureActions", numFailureActions);

	{
		int i;
		for (i = 0; i < numConditions; i += 1) {
			ECharString tmpStr;
			if (2 == version) {
				tmpStr.Format("Objective%dCondition%d", objectiveNum, i);
			} else {
				tmpStr.Format("Team%dObjective%dCondition%d", Alignment(), objectiveNum, i);
			}
			result = missionFile->seekBlock(tmpStr.Data());
			if (NO_ERR != result) { assert(false); continue; }
			condition_species_type species;
			result = sReadIdString(missionFile, "ConditionSpeciesString", tmpECStr);
			if (NO_ERR != result) {
				/* this is just for backward compatibility, this entry is depricated */
				int ispecies = 0;
				result = sReadIdWholeNum(missionFile, "ConditionSpecies", ispecies);
				if (NO_ERR != result) { continue; }
				species = (condition_species_type)ispecies;
			} else {
				species = ConditionSpeciesMap(tmpECStr.Data());
			}

			CObjectiveCondition *pNewObjectiveCondition = new_CObjectiveCondition(species, m_alignment);
			if (!pNewObjectiveCondition) { assert(false); delete pNewObjectiveCondition; continue; }
			bool bresult = pNewObjectiveCondition->Read(missionFile);
			if (true != bresult) { assert(false); delete pNewObjectiveCondition; continue; }
			Append(pNewObjectiveCondition);
		}
	}

	{
		int i;
		for (i = 0; i < numActions; i += 1) {
			ECharString tmpStr;
			tmpStr.Format("Team%dObjective%dAction%d", Alignment(), objectiveNum, i);
			result = missionFile->seekBlock(tmpStr.Data());
			if (NO_ERR != result) { assert(false); continue; }
			action_species_type species;
			result = sReadIdString(missionFile, "ActionSpeciesString", tmpECStr);
			if (NO_ERR != result) { assert(false); continue; }
			species = ActionSpeciesMap(tmpECStr.Data());

			CObjectiveAction *pNewObjectiveAction = new_CObjectiveAction(species, m_alignment);
			if (!pNewObjectiveAction) { assert(false); delete pNewObjectiveAction; continue; }
			bool bresult = pNewObjectiveAction->Read(missionFile);
			if (true != bresult) { assert(false); delete pNewObjectiveAction; continue; }
			m_actionList.Append(pNewObjectiveAction);
		}
	}

	{
		int i;
		for (i = 0; i < numFailureConditions; i += 1) {
			ECharString tmpStr;
			tmpStr.Format("Team%dObjective%dFailureCondition%d", Alignment(), objectiveNum, i);
			result = missionFile->seekBlock(tmpStr.Data());
			if (NO_ERR != result) { assert(false); continue; }
			condition_species_type species;
			result = sReadIdString(missionFile, "FailureConditionSpeciesString", tmpECStr);
			if (NO_ERR != result) { assert(false); continue; }
			species = ConditionSpeciesMap(tmpECStr.Data());

			CObjectiveCondition *pNewObjectiveFailureCondition = new_CObjectiveCondition(species, m_alignment);
			if (!pNewObjectiveFailureCondition) { assert(false); delete pNewObjectiveFailureCondition; continue; }
			bool bresult = pNewObjectiveFailureCondition->Read(missionFile);
			if (true != bresult) { assert(false); delete pNewObjectiveFailureCondition; continue; }
			m_failureConditionList.Append(pNewObjectiveFailureCondition);
		}
	}

	{
		int i;
		for (i = 0; i < numFailureActions; i += 1) {
			ECharString tmpStr;
			tmpStr.Format("Team%dObjective%dFailureAction%d", Alignment(), objectiveNum, i);
			result = missionFile->seekBlock(tmpStr.Data());
			if (NO_ERR != result) { assert(false); continue; }
			action_species_type species;
			result = sReadIdString(missionFile, "FailureActionSpeciesString", tmpECStr);
			if (NO_ERR != result) { assert(false); continue; }
			species = ActionSpeciesMap(tmpECStr.Data());

			CObjectiveAction *pNewObjectiveFailureAction = new_CObjectiveAction(species, m_alignment);
			if (!pNewObjectiveFailureAction) { assert(false); delete pNewObjectiveFailureAction; continue; }
			bool bresult = pNewObjectiveFailureAction->Read(missionFile);
			if (true != bresult) { assert(false); delete pNewObjectiveFailureAction; continue; }
			m_failureActionList.Append(pNewObjectiveFailureAction);
		}
	}
	return true;
}

objective_status_type CObjective::Status() {
	if (!IsActive()) {
		//assert(false);
		return OS_UNDETERMINED;
	}
	if (m_resolved) {
		return m_resolvedStatus;
	} else { 
		objective_status_type objectiveStatus = OS_SUCCESSFUL;
		{
			/*evaluate the status of the success conditions*/
			EIterator it = Begin();
			while (!it.IsDone()) {
				objective_status_type conditionStatus = (*it)->Status();
				if (OS_FAILED == conditionStatus) {
					objectiveStatus = OS_FAILED;
					break;
				} else if (OS_UNDETERMINED == conditionStatus) {
					objectiveStatus = OS_UNDETERMINED;
				}
				it++;
			}
		}
		if ((OS_FAILED != objectiveStatus) && (0 < m_failureConditionList.Count())) {
			/*evaluate the status of the failure conditions*/
			objective_status_type failureStatus = OS_SUCCESSFUL;
			EIterator it = m_failureConditionList.Begin();
			while (!it.IsDone()) {
				objective_status_type conditionStatus = (*it)->Status();
				if (OS_FAILED == conditionStatus) {
					failureStatus = OS_FAILED;
					break;
				} else if (OS_UNDETERMINED == conditionStatus) {
					failureStatus = OS_UNDETERMINED;
					break;
				}
				it++;
			}
			if (OS_SUCCESSFUL == failureStatus) {
				/*the failure conditions are satisfied*/
				objectiveStatus = OS_FAILED;
			}
		}
		if (OS_UNDETERMINED == objectiveStatus) {
			return OS_UNDETERMINED;
		} else if (OS_FAILED == objectiveStatus) {
			m_resolved = true;
			m_changedStatus = true;
			m_resolvedStatus = OS_FAILED;
			if (!IsHiddenTrigger()) {
				soundSystem->playBettySample(BETTY_CANNOT_COMP_OBJ);
			}
			action_list_type::EIterator it = m_failureActionList.Begin();
			while (!it.IsDone()) {
				(*it)->Execute();
				it++;
			}
			return m_resolvedStatus;
		} else {
			m_resolved = true;
			m_changedStatus = true;
			m_resolvedStatus = OS_SUCCESSFUL;
			if (!IsHiddenTrigger()) {
				soundSystem->playBettySample(BETTY_OBJECTIVE_COMPLETE);
			}
			action_list_type::EIterator it = m_actionList.Begin();
			while (!it.IsDone()) {
				(*it)->Execute();
				it++;
			}
			return m_resolvedStatus;
		}
	}
}

objective_status_type CObjective::Status(CObjectives &objectives) {
	if (!IsActive()) {
		//assert(false);
		return OS_UNDETERMINED;
	}

	if ((!PreviousPrimaryObjectiveMustBeComplete()) && !(AllPreviousPrimaryObjectivesMustBeComplete())) {
		return Status();
	}

	objective_status_type retval = OS_SUCCESSFUL;
	objective_status_type statusOfPreviousPrimaryObjective = OS_SUCCESSFUL;
	bool allPreviousPrimaryObjectivesHaveBeenCompleted = true;
	CObjectives::EIterator it = objectives.Begin();
	while (!it.IsDone()) {
		objective_status_type status;
		if (((*it)->AllPreviousPrimaryObjectivesMustBeComplete()) && (!allPreviousPrimaryObjectivesHaveBeenCompleted)) {
			status = OS_UNDETERMINED;
		} else if (((*it)->PreviousPrimaryObjectiveMustBeComplete()) && (OS_SUCCESSFUL != statusOfPreviousPrimaryObjective)) {
			assert(!(*it)->AllPreviousPrimaryObjectivesMustBeComplete());
			assert(OS_UNDETERMINED == statusOfPreviousPrimaryObjective);
			status = statusOfPreviousPrimaryObjective;
		} else {
			status = (*it)->Status();
		}
		if (this == (*it)) {
			return status;
		}
		assert (OS_FAILED != retval);
		if (1 == (*it)->Priority()) {
			if (OS_FAILED ==  status) {
				allPreviousPrimaryObjectivesHaveBeenCompleted = false;
				retval = OS_FAILED;
				/* if one primary objective is failed, it's over */
				break;
			} else if (OS_UNDETERMINED == status) {
				allPreviousPrimaryObjectivesHaveBeenCompleted = false;
				retval = OS_UNDETERMINED;
			}
			statusOfPreviousPrimaryObjective = status;
		}
		it++;
	}
	return retval;

}

void CObjective::Status(objective_status_type newStatus) {
	if (OS_UNDETERMINED == newStatus) {
		m_resolved = false;
	} else {
		m_resolved = true;
	}
	m_resolvedStatus = newStatus;
	m_changedStatus = true;
}

bool CObjective::StatusChangedSuccess (void)
{
	if (!IsActive())
	{
		return false;
	}
	
	if (m_changedStatus)
	{
		m_changedStatus = false;
		m_resolvedStatus;
		if (m_resolvedStatus == OS_SUCCESSFUL)
			return true;
	}
	
	return false;
}
									  
bool CObjective::StatusChangedFailed (void)
{
	if (!IsActive())
	{
		return false;
	}
	
	if (m_changedStatus)
	{
		m_changedStatus = false;
		if (m_resolvedStatus == OS_FAILED)
			return true;
	}
	
	return false;
}

EString CObjective::LocalizedTitle() const {
	EString retval;
	if (TitleUseResourceString()) {
		EString EStr;
		ESLoadString(TitleResourceStringID(), EStr);
		retval = EStr.Data();
	} else {
		retval = Title();
	}
	return retval;
}

EString CObjective::LocalizedDescription() const {
	EString retval;
	if (DescriptionUseResourceString()) {
		EString EStr;
		ESLoadString(DescriptionResourceStringID(), EStr);
		retval = EStr.Data();
	} else {
		retval = Description();
	}
	return retval;
}

CObjectives &CObjectives::operator=(const CObjectives &master) {
	m_alignment = master.m_alignment;

	Clear();
	CObjectives::EConstIterator it = master.Begin();
	while (!it.IsDone()) {
		//CObjective *tmpObjective = new CObjective(*(*it));
		CObjective *tmpObjective = new CObjective(m_alignment);
		(*tmpObjective) = (*(*it));
		Append(tmpObjective);
		it++;
	}
	return (*this);
}

void CObjectives::Clear() {
	EIterator it = Begin();
	while (!it.IsDone()) {
		delete (*it);
		it++;
	}
	inherited::Clear();
	boolFlags.Clear();
}

void CObjectives::Alignment(int alignment) {
	m_alignment = alignment;
	EIterator it = Begin();
	while (!it.IsDone()) {
		(*it)->Alignment(m_alignment);
		it++;
	}
}

bool CObjectives::Read( FitIniFile* missionFile )
{
	long result = 0;
	result = missionFile->seekBlock("Objectives Version");
	if (NO_ERR != result) { assert(false); }
	int objectivesVersion = 0;
	result = sReadIdWholeNum(missionFile, "Version", objectivesVersion);
	if (result != NO_ERR) 
	{
		assert(false); return false;
	} 
	else 
	{
		if (2 == objectivesVersion) 
		{
			if (0 != Alignment()) 
			{
				// assign version 2 objectives to team1 only
				return true;
			}
			result = missionFile->seekBlock("Objectives");
		} 
		else 
		{
			ECharString tmpStr;
			tmpStr.Format("Team%dObjectives", Alignment());
			result = missionFile->seekBlock(tmpStr.Data());
		}

		if (NO_ERR != result) 
		{ 
			assert(false); 
		}

		int numObjectives = 0;
		result = sReadIdWholeNum(missionFile, "NumObjectives", numObjectives);
		if (NO_ERR != result) 
		{ 
			return false; 
		}

		boolFlags.load(Alignment(),missionFile);

		int i;
		int markerI = 1;
		char secondaryMarkers = 'a';
		for (i = 0; i < numObjectives; i += 1) {
			ECharString tmpStr;
			if (2 == objectivesVersion) {
				tmpStr.Format("Objective%d", i);
			} else {
				tmpStr.Format("Team%dObjective%d", Alignment(), i);
			}
			result = missionFile->seekBlock(tmpStr.Data());
			if (NO_ERR != result) { assert(false); continue; }
			CObjective *pNewObjective = new CObjective(m_alignment);
			bool bresult = pNewObjective->Read(missionFile, i, objectivesVersion, markerI, secondaryMarkers);
			
			if ( 1 == pNewObjective->Priority() )
				markerI++;
			else
				secondaryMarkers++;
			
			if (true != bresult) { assert(false); delete pNewObjective; continue; }
			Append(pNewObjective);
		}
	}
	return true;
}

objective_status_type CObjectives::Status() {
	objective_status_type retval = OS_SUCCESSFUL;
	objective_status_type statusOfPreviousPrimaryObjective = OS_SUCCESSFUL;
	bool allPreviousPrimaryObjectivesHaveBeenCompleted = true;
	EIterator it = Begin();
	while (!it.IsDone()) {
		if ((*it)->IsActive()) {
			/* check if objective status needs to be reset */
			if ((*it)->ResetStatusOnFlag()) {
				if (Team::teams[Alignment()]->objectives.boolFlags.getElementValue((*it)->ResetStatusFlagID())) {
					Team::teams[Alignment()]->objectives.boolFlags.setElementValue((*it)->ResetStatusFlagID(), false);
					(*it)->Status(OS_UNDETERMINED);
					//mission->missionInterface->showObjectives(true);
				}
			}
			objective_status_type status;
			if (((*it)->AllPreviousPrimaryObjectivesMustBeComplete()) && (!allPreviousPrimaryObjectivesHaveBeenCompleted)) {
				status = OS_UNDETERMINED;
			} else if (((*it)->PreviousPrimaryObjectiveMustBeComplete()) && (OS_SUCCESSFUL != statusOfPreviousPrimaryObjective)) {
				assert(!(*it)->AllPreviousPrimaryObjectivesMustBeComplete());
				assert(OS_UNDETERMINED == statusOfPreviousPrimaryObjective);
				status = statusOfPreviousPrimaryObjective;
			} else {
				status = (*it)->Status();
			}
			assert (OS_FAILED != retval);
			if (1 == (*it)->Priority()) {
				if (OS_FAILED ==  status) {
					allPreviousPrimaryObjectivesHaveBeenCompleted = false;
					retval = OS_FAILED;
					/* if one primary objective is failed, it's over */
					break;
				} else if (OS_UNDETERMINED == status) {
					allPreviousPrimaryObjectivesHaveBeenCompleted = false;
					retval = OS_UNDETERMINED;
				}
				statusOfPreviousPrimaryObjective = status;
			}
		} else {
			/* check if inactive objective needs to become active */
			if ((*it)->ActivateOnFlag()) {
				if (1 == (*it)->Priority()) {
					/* do not end the mission if a primary objective is still inactive */
					retval = OS_UNDETERMINED;
				}
				//if (mission->missionObjectives.boolFlags.getElementValue((*it)->ActivateFlagID())) {
				if (Team::teams[Alignment()]->objectives.boolFlags.getElementValue((*it)->ActivateFlagID())) {
					(*it)->IsActive(true);
					(*it)->ActivationTime(mission->actualTime);
					// PORT INSTRUMENT (M8-B65): dump again the moment an objective
					// unlocks, so the extraction objective's own activation frame
					// shows whether its marker building is still on the map.
					{
						extern void MC2_ObjectiveCensus (const char* when);
						MC2_ObjectiveCensus("objective activated");
					}
					mission->missionInterface->showObjectives(true);
					//mission->missionInterface->controlGui.handleClick(OBJECTIVES_COMMAND);
				}
			}
		}
		it++;
	}

	// M8-B65: keep the nav-marker props in step with what the player still has to
	// do. Cheap -- a handful of markers against a handful of objectives.
	{
		extern void MC2_UpdateNavMarkers (CObjectives* objectives);
		MC2_UpdateNavMarkers(this);
	}

	return retval;
}

/*void CObjective::Render( unsigned long xPos, unsigned long yPos, HGOSFONT3D guiFont )
{
	unsigned long fontWidth, fontHeight;
	gos_TextStringLength( &fontWidth, &fontHeight, "ABC" );
	
	// draw little box first
	GUI_RECT rect = { xPos, yPos, xPos + fontHeight, yPos + fontHeight };
	drawEmptyRect( rect, 0xffffffff, 0xffffffff );

	unsigned long color = 0xffffffff;

	if ( m_resolved )
	{
		color = m_resolvedStatus == OS_FAILED ?  0xffff0000 : 0xff00ff00;
		gos_TextSetAttributes( guiFont, color, 1.0, false, true, false, false, 0 );	
		gos_TextSetPosition(xPos + (fontWidth >> 1), yPos );

		if ( m_resolvedStatus == OS_FAILED )
			gos_TextDraw( "X" );
		else if ( m_resolvedStatus == OS_SUCCESSFUL )
		{
			gos_TextDraw( "V" );
		}	
	}
	
	gos_TextSetAttributes( guiFont, color, 1.0, false, true, false, false, 0 );	
	gos_TextSetPosition(xPos + 2 * fontWidth, yPos );

	gos_TextDraw( (const char*)m_description );

}*/

bool CObjective::RenderMarkers(GameTacMap *tacMap, bool blink ) 
{
	bool bRetVal = 0;
	if (m_resolved || !m_displayMarker || !m_isActive)
	{
		//Draw Nothing.  Objective is resolved or hidden
	} 
	else 
	{
			Stuff::Vector3D objectivePos;
			objectivePos.x = m_markerX;
			objectivePos.y = m_markerY;
			long color = SD_YELLOW;

			if ( m_displayMarker == NAV )
			{
				bRetVal = 1; // true if we drew something

				if ( blink )
				{
					s_lastBlinkTime += frameLength;
					if ( s_lastBlinkTime > s_blinkLength )
					{
						s_lastBlinkTime = 0.f;
						if ( !s_blinkColor )
							s_blinkColor = SD_YELLOW;
						else
							s_blinkColor = 0;
					}
					color = s_blinkColor;

				}

			}
			else
			{
				color = 0xffff0000;
				gos_VERTEX pos;
				tacMap->worldToTacMap(objectivePos, pos);

				unsigned long width, height;
				height = s_markerFont->height();
				width = s_markerFont->width( m_markerText );
				drawShadowText( 0xffffffff, 0xff000000, s_markerFont->getTempHandle(), pos.x - width/2, pos.y - height/2, true, m_markerText, 0, s_markerFont->getSize(), -2, 2 );

			}

	}

	return bRetVal;
}


/* Reads Nav Marker info and adds appropriate "hidden trigger" objectives. */
bool ReadNavMarkers( FitIniFile* missionFile, CObjectives &objectives )
{
	long result = 0;
	result = missionFile->seekBlock("NavMarkers");
	if (NO_ERR != result) { /*assert(false);*/ return false; }
	int numNavMarkers = 0;
	result = sReadIdWholeNum(missionFile, "NumNavMarkers", numNavMarkers);
	if (NO_ERR != result) { return false; }
	int i;
	for (i = 0; i < numNavMarkers; i += 1) {
		ECharString tmpStr;
		tmpStr.Format("NavMarker%d", i);
		result = missionFile->seekBlock(tmpStr.Data());
		if (NO_ERR != result) { assert(false); continue; }
		CObjective *pNewObjective = new CObjective(objectives.Alignment());

		ECharString tmpECStr;
		tmpECStr.Format("Nav Marker %d", i);
		pNewObjective->Title(tmpECStr.Data());
		pNewObjective->Priority(2);
		pNewObjective->IsHiddenTrigger(true);
		pNewObjective->DisplayMarker( 0 );

		float targetCenterX, targetCenterY, targetRadius;
		result = sReadIdFloat(missionFile, "xPos", targetCenterX);
		if (NO_ERR != result) { assert(false); delete pNewObjective; continue; }
		result = sReadIdFloat(missionFile, "yPos", targetCenterY);
		if (NO_ERR != result) { assert(false); delete pNewObjective; continue; }
		result = sReadIdFloat(missionFile, "radius", targetRadius);
		if (NO_ERR != result) { assert(false); delete pNewObjective; continue; }
		pNewObjective->MarkerX( targetCenterX );
		pNewObjective->MarkerY( targetCenterY );

		CMoveAnyUnitToArea *pCondition = new CMoveAnyUnitToArea(pNewObjective->Alignment());
		pCondition->SetParams(targetCenterX, targetCenterY, targetRadius);
		pNewObjective->Append(pCondition);

		C_RemoveStructure *pAction = new C_RemoveStructure(pNewObjective->Alignment());
		bool bresult = pAction->SetParams(targetCenterX, targetCenterY);
		// M8-B65: remember the area so the marker can come back if a later
		// objective sends the player into it again.
		pAction->SetArea(targetCenterX, targetCenterY, targetRadius);
		// PORT INSTRUMENT (M8-B65): SetParams matches a building by EXACT float
		// equality on x and y. When it misses, the release build's assert is a
		// no-op and the WHOLE nav-marker objective is silently thrown away --
		// which is one of the two candidate causes for the missing extraction
		// marker. Say which happened, opt-in, so the two can be told apart.
		{
			const char* want = getenv("MC2_OBJECTIVE_CENSUS");
			if (want && want[0] && strcmp(want, "0"))
				printf("[objective-census] navmarker%d at (%.6f,%.6f) r=%.1f -> %s\n",
					i, targetCenterX, targetCenterY, targetRadius,
					bresult ? "matched a building" : "NO BUILDING MATCHED (objective discarded)");
		}
		if (true != bresult) { assert(false); delete pAction; delete pNewObjective; continue; }
		pNewObjective->m_actionList.Append(pAction);

		objectives.Append(pNewObjective);
	}
	return true;
}

// M8-B65. Nav markers that have been consumed, so the map can be corrected when
// a later objective points back into one of their areas. A PLAIN ARRAY on
// purpose: an EList here destructs after the game heap is gone and takes the
// process down inside UserHeap::Free at exit -- measured, exit code 5, before
// this was changed. Missions have a handful of nav markers; 32 is ample and the
// overflow is simply ignored rather than grown.
static C_RemoveStructure* s_navMarkers[32];
static long               s_navMarkerCount = 0;

int C_RemoveStructure::Execute()
{
	Building* pBuilding = (Building*)ObjectManager->getByWatchID(m_pBuildingWID);
	if (!pBuilding)
		return true;

	if (!m_wasRemoved)
	{
		m_savedPosition = pBuilding->position;

		bool known = false;
		for (long i = 0; i < s_navMarkerCount; i++)
			if (s_navMarkers[i] == this)
				known = true;
		if (!known && (s_navMarkerCount < 32))
			s_navMarkers[s_navMarkerCount++] = this;
	}

	RemoveNow();
	return true;
}

void C_RemoveStructure::RemoveNow()
{
	Building* pBuilding = (Building*)ObjectManager->getByWatchID(m_pBuildingWID);
	if (!pBuilding)
		return;

	Stuff::Vector3D ludicrousPos;
	ludicrousPos.x = 100000.f;
	ludicrousPos.y = 100000.f;
	ludicrousPos.z = 100000.f;

	pBuilding->position = ludicrousPos;
	pBuilding->update();
	m_wasRemoved = true;
}

void C_RemoveStructure::PlaceAt (const Stuff::Vector3D& where, long asObjectNumber)
{
	Building* pBuilding = (Building*)ObjectManager->getByWatchID(m_pBuildingWID);
	if (!pBuilding)
		return;

	// M8-B65c. mc2_20 ships ONE marker prop and it is a "Nav Marker" (object 290).
	// The extraction objective asks for object 289, "Extraction Marker" -- a real
	// object type, used in mc2_22, that this mission simply never places. Rather
	// than find a free slot in the fixed building pool, the prop we already own is
	// re-skinned into the one the objective names. Building::init rebuilds the
	// appearance from the type, which is exactly the work required.
	ObjectTypePtr wanted = NULL;
	if (asObjectNumber > 0)
	{
		const long have = pBuilding->getObjectType()
						 ? pBuilding->getObjectType()->getObjTypeNum() : -1;
		if (have != asObjectNumber)
		{
			wanted = ObjectManager->loadObjectType(asObjectNumber);
			if (!wanted)
				wanted = ObjectManager->getObjectType(asObjectNumber);
			// Only ever swap one building for another. An objective whose model is a
			// mech or a vehicle must not turn a marker into one.
			if (wanted && (wanted->getObjectTypeClass() != BUILDING_TYPE))
				wanted = NULL;
		}
	}

	if (wanted)
	{
		if (pBuilding->appearance)
		{
			delete pBuilding->appearance;
			pBuilding->appearance = NULL;
		}
		pBuilding->init(false, wanted);
	}

	pBuilding->position = where;
	pBuilding->update();
	m_wasRemoved = false;

	const char* want = getenv("MC2_OBJECTIVE_CENSUS");
	if (want && want[0] && strcmp(want, "0"))
		printf("[objective-census] marker placed at (%.1f,%.1f) as object %ld%s; it "
			   "started at (%.1f,%.1f)\n",
			where.x, where.y,
			pBuilding->getObjectType() ? (long)pBuilding->getObjectType()->getObjTypeNum() : -1L,
			wanted ? " (re-skinned)" : "",
			m_savedPosition.x, m_savedPosition.y);
}

bool C_RemoveStructure::PointsHere (float x, float y) const
{
	if (m_areaRadius <= 0.0f)
		return false;

	const float dx = x - m_areaX;
	const float dy = y - m_areaY;
	return ((dx * dx + dy * dy) <= (m_areaRadius * m_areaRadius));
}

// M8-B65. A nav marker's structure belongs on the map exactly while some
// objective the player can still be sent to points into its area -- the same
// three tests RenderMarkers uses to decide whether to draw that objective's
// marker at all. Reaching a nav marker still clears it; what changes is that
// mission 20's extraction objective, whose target sits 171 units inside
// NavMarker0's own 256 radius, gets its marker back instead of being sent to an
// empty patch of ground. Evaluated every pass, so the order objectives resolve
// in cannot matter -- the first attempt keyed off the activation event and the
// nav marker happened to fire immediately afterwards, undoing it.
void MC2_UpdateNavMarkers (CObjectives* objectives)
{
	if (!objectives || (s_navMarkerCount == 0))
		return;

	for (long i = 0; i < s_navMarkerCount; i++)
	{
		C_RemoveStructure* pMarker = s_navMarkers[i];
		// Not reached yet? Retail's business. Reaching a nav marker is SUPPOSED
		// to clear it, and it must be allowed to.
		if (!pMarker)
			continue;
		if (pMarker->IsRemoved() == false && pMarker->PlacedForIndex() < 0)
			continue;

		// M8-B65d. WHICH objective the marker comes back for is the whole bug the
		// owner hit: taking the FIRST match put a SECOND DAVION DROPSHIP on the
		// map, because objective 1 "Goto Davion Dropship" also points into this
		// area and its model is the dropship (625). It is reachable from the
		// moment the mission starts, so it was always the first match.
		//
		// The marker is for an objective the player has been sent back here BY --
		// one that was LOCKED at mission start and opened later. ActivateOnFlag is
		// exactly that property, straight from the mission data, and objective 1
		// can never qualify. Highest index wins among those, re-checked every pass
		// so activation ORDER cannot decide the answer.
		//
		// NOT ActivationTime > 0: that was tried and it is a TIMING test. It works
		// in play, where the unlock happens minutes in, and fails silently in any
		// headless run where the unlock lands at t=0 -- so the one check that could
		// verify it cheaply was the one it broke.
		CObjective* best = NULL;
		long bestIndex = -1;
		long index = 0;
		for (EList<CObjective*, CObjective*>::EIterator it = objectives->Begin();
			 !it.IsDone(); it++, index++)
		{
			CObjective* pObj = *it;
			if (!pObj || pObj->IsResolved() || !pObj->DisplayMarker() || !pObj->IsActive())
				continue;
			if (!pObj->ActivateOnFlag())
				continue;
			if (!pMarker->PointsHere(pObj->MarkerX(), pObj->MarkerY()))
				continue;
			best = pObj;
			bestIndex = index;
		}

		if (!best || (bestIndex == pMarker->PlacedForIndex()))
			continue;

		Stuff::Vector3D at;
		at.x = best->MarkerX();
		at.y = best->MarkerY();
		at.z = 0.0f;
		if (land)
			at.z = land->getTerrainElevation(at);
		pMarker->PlaceAt(at, best->ModelID());
		pMarker->PlacedForIndex(bestIndex);
	}
}

//*************************************************************************************************
bool CNumberOfUnitsObjectiveCondition::Save( FitIniFile* file )
{
	file->writeIdULong( "Num", m_num );
	return true;
}

bool CSpecificUnitObjectiveCondition::Save( FitIniFile* file )
{
	if (ObjectManager->getByWatchID(m_pUnitWID) == NULL)
		STOP(("Tried to save specific unit objective with NULL unit"));

	file->writeIdLong( "MoverWID", m_pUnitWID );

	return true;
}

bool C_RemoveStructure::Save( FitIniFile* file )
{
	if (ObjectManager->getByWatchID(m_pBuildingWID) == NULL)
		STOP(("Tried to save specific Building objective with NULL Building"));

	file->writeIdLong( "BuildingWID", m_pBuildingWID );

	return true;
}

bool CSpecificStructureObjectiveCondition::Save( FitIniFile* file )
{
	if (ObjectManager->getByWatchID(m_pBuildingWID) == NULL)
		STOP(("Tried to save specific Bduiling objective with NULL Building"));

	file->writeIdLong( "BuildingWID", m_pBuildingWID );

	return true;
}

bool CAreaObjectiveCondition::Save( FitIniFile* file )
{
	file->writeIdFloat( "TargetCenterX", m_targetCenterX );
	file->writeIdFloat( "TargetCenterY", m_targetCenterY );
	file->writeIdFloat( "TargetRadius", m_targetRadius );
	return true;
}

bool CBooleanFlagIsSet::Save( FitIniFile* file )
{
	file->writeIdString("FlagID", m_flagID.Data());
	file->writeIdBoolean("Value", m_value);
	return true;
}

bool CElapsedMissionTime::Save( FitIniFile* file )
{
	file->writeIdFloat("Time", m_time);
	return true;
}

bool CPlayBIK::Save( FitIniFile* file )
{
	file->writeIdString( "AVIFileName", m_pathname.Data());
	return true;
}

bool CPlayWAV::Save( FitIniFile* file )
{
	file->writeIdString( "WAVFileName", m_pathname.Data());
	return true;
}

bool CDisplayTextMessage::Save( FitIniFile* file )
{
	file->writeIdString( "Message", m_message.Data());
	return true;
}

bool CDisplayResourceTextMessage::Save( FitIniFile* file )
{
	file->writeIdULong( "ResourceStringID", m_resourceStringID);
	return true;
}

bool CSetBooleanFlag::Save( FitIniFile* file )
{
	file->writeIdString("FlagID", m_flagID.Data());
	file->writeIdBoolean("Value", m_value);
	return true;
}

bool CMakeNewTechnologyAvailable::Save( FitIniFile* file )
{
	file->writeIdString( "PurchaseFileName", m_purchaseFilePathname.Data());
	return true;
}

bool CObjective::Save( FitIniFile* file, int objectiveNum )
{
	file->writeIdString( "Title", Title().Data());
	file->writeIdBoolean( "TitleUseResourceString", m_titleUseResourceString );
	file->writeIdULong( "TitleResourceStringID", m_titleResourceStringID );
	file->writeIdString( "Description", Description().Data());
	file->writeIdBoolean( "DescriptionUseResourceString", m_descriptionUseResourceString );
	file->writeIdULong( "DescriptionResourceStringID", m_descriptionResourceStringID );
	file->writeIdULong( "Priority", m_priority );
	file->writeIdULong( "ResourcePoints", m_resourcePoints );
	file->writeIdBoolean( "PreviousPrimaryObjectiveMustBeComplete", m_previousPrimaryObjectiveMustBeComplete );
	file->writeIdBoolean( "AllPreviousPrimaryObjectivesMustBeComplete", m_allPreviousPrimaryObjectivesMustBeComplete );
	file->writeIdBoolean( "DisplayMarker", m_displayMarker );
	file->writeIdFloat( "MarkerX", m_markerX );
	file->writeIdFloat( "MarkerY", m_markerY );
	file->writeIdBoolean( "HiddenTrigger", m_isHiddenTrigger );
	file->writeIdBoolean( "ActivateOnFlag", m_activateOnFlag );
	file->writeIdString( "ActivateFlagID", m_activateFlagID.Data() );
	file->writeIdBoolean( "ResetStatusOnFlag", m_resetStatusOnFlag );
	file->writeIdString( "ResetStatusFlagID", m_resetStatusFlagID.Data() );

	file->writeIdULong( "NumConditions", Count() );
	file->writeIdULong( "NumActions", m_actionList.Count() );
	file->writeIdULong( "NumFailureConditions", m_failureConditionList.Count() );
	file->writeIdULong( "NumFailureActions", m_failureActionList.Count() );

	if (m_modelName.Data())
		file->writeIdString( "ObjectiveModel", m_modelName.Data() );

	file->writeIdLong( "ModelType", m_modelType );
	file->writeIdLong( "BaseColor", m_modelBaseColor );
	file->writeIdLong( "HighlightColor", m_modelHighlightColor );
	file->writeIdLong( "HighlightColor2", m_modelHighlightColor2 );
//	file->writeIdLong( "ModelID", m_modelID );
	file->writeIdFloat( "ModelScale", m_modelScale );

	/*state data*/
	file->writeIdBoolean( "IsActive", m_isActive );
	file->writeIdFloat( "ActivationTime", m_activationTime );
	file->writeIdBoolean( "IsResolved", m_resolved );
	file->writeIdLong( "ResolvedStatus", (int)m_resolvedStatus );
	file->writeIdBoolean( "ChangedStatus", m_changedStatus );
	/*end state data*/

	{
		int i = 0;
		EIterator it = Begin();
		while (!it.IsDone()) {
			ECharString tmpStr;
			tmpStr.Format("Team%dObjective%dCondition%d", Alignment(), objectiveNum, i);
			file->writeBlock( tmpStr.Data() );
			file->writeIdULong( "ConditionSpecies", (unsigned long)(*it)->Species());
			file->writeIdString( "ConditionSpeciesString", g_conditionSpeciesStringArray[(int)(*it)->Species()]);
			(*it)->Save(file);
			i += 1;
			it++;
		}
	}

	{
		int i = 0;
		action_list_type::EIterator it = m_actionList.Begin();
		while (!it.IsDone()) {
			ECharString tmpStr;
			tmpStr.Format("Team%dObjective%dAction%d", Alignment(), objectiveNum, i);
			file->writeBlock( tmpStr.Data() );
			file->writeIdULong( "ActionSpecies", (unsigned long)(*it)->Species());
			file->writeIdString( "ActionSpeciesString", g_actionSpeciesStringArray[(int)(*it)->Species()]);
			(*it)->Save(file);
			i += 1;
			it++;
		}
	}

	{
		int i = 0;
		condition_list_type::EIterator it = m_failureConditionList.Begin();
		while (!it.IsDone()) {
			ECharString tmpStr;
			tmpStr.Format("Team%dObjective%dFailureCondition%d", Alignment(), objectiveNum, i);
			file->writeBlock( tmpStr.Data() );
			file->writeIdULong( "FailureConditionSpecies", (unsigned long)(*it)->Species());
			file->writeIdString( "FailureConditionSpeciesString", g_conditionSpeciesStringArray[(int)(*it)->Species()]);
			(*it)->Save(file);
			i += 1;
			it++;
		}
	}

	{
		int i = 0;
		action_list_type::EIterator it = m_failureActionList.Begin();
		while (!it.IsDone()) {
			ECharString tmpStr;
			tmpStr.Format("Team%dObjective%dFailureAction%d", Alignment(), objectiveNum, i);
			file->writeBlock( tmpStr.Data() );
			file->writeIdULong( "FailureActionSpecies", (unsigned long)(*it)->Species());
			file->writeIdString( "FailureActionSpeciesString", g_actionSpeciesStringArray[(int)(*it)->Species()]);
			(*it)->Save(file);
			i += 1;
			it++;
		}
	}

	return true;
}

bool CObjectives::Save( FitIniFile* file )
{
 	file->writeBlock("Objectives Version");
	file->writeIdULong("Version", 3);

	ECharString tmpStr;
	tmpStr.Format("Team%dObjectives", Alignment());
	file->writeBlock( tmpStr.Data() );
	file->writeIdULong( "NumObjectives", Count() );
	int i = 0;

	boolFlags.save(Alignment(),file);

	EIterator it = Begin();
	while (!it.IsDone()) {
		ECharString tmpStr;
		tmpStr.Format("Team%dObjective%d", Alignment(), i);
		file->writeBlock( tmpStr.Data() );
		(*it)->Save(file, i);
		i += 1;
		it++;
	}

	return true;
}

//*************************************************************************************************
// end of file ( Objective.cpp )

//*************************************************************************************************
// PORT INSTRUMENT -- M8-B65 / M8-B66, opt-in via MC2_OBJECTIVE_CENSUS.
//
// Two owner reports from mission mc2_20 ("Combat Recovery: Dropship") need a
// measurement before anything is edited:
//
//   M8-B65  the extraction marker never appears when objective 6 activates.
//   M8-B66  the prison only captures from one particular side.
//
// Reading the data got as far as two COMPETING explanations for B65 and no way
// to choose between them from the text:
//
//   (a) the in-world marker is a BUILDING that ReadNavMarkers arms for removal,
//       and it is consumed early -- NavMarker0 sits at (-1429.3, 490.7) with a
//       256 radius, while objective 1 makes you drive to (-1258, 533). If that
//       is it, the census finds the building at mission start and finds it gone
//       (position 100000) after the player has been there.
//
//   (b) C_RemoveStructure::SetParams matches the building by EXACT float
//       equality on x and y against a value the .fit stores to six decimals. If
//       that comparison misses, the whole nav-marker objective is discarded at
//       load and no building is ever armed. If that is it, the census reports
//       navmarker=NO-BUILDING at mission start and the marker never moves.
//
// Those two predict different output, which is the point of printing it.
//
// For B66 the question is what the capture test in tacordr.cpp actually
// measures: it needs the mech within 30.0 of ONE point, target->getPosition().
// If the prison's position is its centre and its extent radius is well over 30,
// no approach can satisfy it and the ones that work do so by accident of where
// the pathfinder parks; if the position is off-centre, the reachable side is the
// near one. The census prints position, extent radius and footprint so the
// difference is visible rather than argued.
//
// Always safe to leave in: it prints nothing unless the variable is set, and it
// reads state without touching it.
//*************************************************************************************************

// M8-B70. A building a mech walks through is not blocking its cell. Ask the
// movement map directly -- that is what the pathfinder reads -- rather than
// inferring solidity from the object.
// M8-B70. How far the BLOCKED ground around a building actually reaches --
// marched outward on eight bearings until the map turns passable, and the
// largest contiguous radius reported. Compared against the object extent this
// says whether a mech can stand inside the visible model.
// M8-B70, corrected. The first version marched outward from the centre and
// stopped at the first passable cell -- which reads 0 whenever the centre cell
// itself is open, and markMoveMap marks by VERTEX, so the middle of a building
// often has no vertex over it and stays open while its edges are blocked.
// Counting blocked cells in a neighbourhood cannot be fooled that way.
static long MC2_BlockedCellsNear (const Stuff::Vector3D& c)
{
	if (!GameMap || !land)
		return -1;
	long row = 0, col = 0;
	land->worldToCell(c, row, col);
	long blocked = 0;
	for (long dr = -3; dr <= 3; dr++)
		for (long dc = -3; dc <= 3; dc++)
		{
			const long r = row + dr, cc = col + dc;
			if (GameMap->inBounds(r, cc) && !GameMap->getPassable(r, cc))
				blocked++;
		}
	return blocked;
}

static int MC2_CellPassableAt (const Stuff::Vector3D& pos)
{
	if (!GameMap || !land)
		return -1;
	long row = 0, col = 0;
	land->worldToCell(pos, row, col);
	if (!GameMap->inBounds(row, col))
		return -1;
	return GameMap->getPassable(row, col) ? 1 : 0;
}

void MC2_ObjectiveCensus (const char* when)
{
	const char* want = getenv("MC2_OBJECTIVE_CENSUS");
	if (!want || !want[0] || !strcmp(want, "0"))
		return;

	printf("[objective-census] --- %s ---\n", when ? when : "?");

	if (Team::home)
	{
		long index = 0;
		for (EList<CObjective*, CObjective*>::EIterator iter = Team::home->objectives.Begin();
			 !iter.IsDone(); iter++, index++)
		{
			CObjective* pObj = *iter;
			if (!pObj)
				continue;
			printf("[objective-census] obj%ld active=%d hidden=%d marker=%d markerXY=(%.1f,%.1f)"
				   " status=%d activateOnFlag=%d flag=%s title=\"%s\"\n",
				index, (int)pObj->IsActive(), (int)pObj->IsHiddenTrigger(), pObj->DisplayMarker(),
				pObj->MarkerX(), pObj->MarkerY(), (int)pObj->Status(),
				(int)pObj->ActivateOnFlag(), pObj->ActivateFlagID().Data(),
				pObj->Title().Data());
		}
	}
	else
		printf("[objective-census] NO HOME TEAM\n");

	// Every building, with the two numbers the capture test and the nav-marker
	// removal actually use. Buildings that have been teleported out by
	// C_RemoveStructure land at 100000 and are labelled so they cannot be missed.
	if (ObjectManager)
	{
		long shown = 0;
		for (long i = 0; i < ObjectManager->getNumBuildings(); i++)
		{
			BuildingPtr pB = ObjectManager->getBuilding(i);
			if (!pB)
				continue;
			Stuff::Vector3D p = pB->getPosition();
			const bool removed = (p.x > 99999.0f);
			printf("[objective-census] bldg%ld type=%ld pos=(%.4f,%.4f,%.1f) extent=%.1f team=%ld"
				   " capturable=%d awake=%d destroyed=%d dmg=%.2f exists=%d tangible=%d passable=%d dmgLevel=%.1f nameId=%ld blocked49=%ld%s\n",
				i, pB->getObjectType() ? (long)pB->getObjectType()->getObjTypeNum() : -1,
				p.x, p.y, p.z, pB->getExtentRadius(), (long)pB->getTeamId(),
				(int)pB->getFlag(OBJECT_FLAG_CAPTURABLE), (int)pB->getAwake(),
				(int)pB->isDestroyed(), pB->getDamage(), (int)pB->getExists(),
				(int)pB->getTangible(), MC2_CellPassableAt(p),
				pB->getObjectType() ? ((BuildingTypePtr)pB->getObjectType())->getDamageLevel() : -1.0f,
				pB->getObjectType() ? (long)((BuildingTypePtr)pB->getObjectType())->buildingTypeName : -1L,
				MC2_BlockedCellsNear(p),
				removed ? "  <-- REMOVED" : "");
			shown++;
		}
		printf("[objective-census] buildings=%ld\n", shown);
	}
	else
		printf("[objective-census] NO OBJECT MANAGER\n");
}

//*************************************************************************************************
// PORT PROBE -- M8-B65 / M8-B66, opt-in via MC2_MISSION_PROBE.
//
// The owner should not have to replay a forty-minute mission to take a
// measurement a machine can take in twenty seconds. This reproduces the two
// situations under test headlessly.
//
// M8-B66, HOW CLOSE CAN A MECH ACTUALLY GET? The capture gate is
//   distanceFrom(target->getPosition()) < 30.0
// which is 30 METRES, and metersPerWorldUnit is 0.2f, so 150 world units from
// the building's centre point. A mech does not stop at the wall, it stops on the
// nearest cell the map lets it stand on -- so the honest question is what that
// distance is ON EACH BEARING. Marching out from the centre until the map turns
// passable answers it per side, and the answer can come back "every side is
// inside the gate", which would clear the gate and send the search elsewhere.
//
// M8-B65, IS THE MARKER EATEN ON ARRIVAL? A nav marker is a building that
// C_RemoveStructure teleports off the map when any unit enters its radius.
// NavMarker0 is 176 world units from objective 1's target, inside its own 256.
// Putting a friendly mover on that target is exactly what the player does at the
// start of the mission; the objective system then runs normally and the census
// prints whether the marker survived. Position is all CMoveAnyUnitToArea reads,
// so a teleport is a faithful stand-in for walking there.
//*************************************************************************************************

// M8-B66. The earlier probe marched outward from a building and took the first
// standable cell. That answers "is there ground there", not "can a mech GET
// there" -- and the prison in mc2_20 sits inside a fenced compound, so close
// ground may be enclosed and useless. This floods the passable cells inward
// from OUTSIDE and reports the closest one a mech could actually walk to,
// which is the distance the capture gate has to live with.
// M8-B66, CORRECTED. The owner is right that blockers cannot explain a
// side-dependent refusal: getCaptureBlocker measures from the BUILDING, so it
// returns the same answer for every mech. They also say the FENCE side is the
// one that refuses. So the question is how close a mech can actually get ON
// EACH SIDE -- not globally, and not by marching a straight line that walks
// through the fence as if it were not there, which is what the first probe did.
//
// One flood from outside the compound, bucketed by bearing: for each octant,
// the closest cell a mech could actually walk to from outside.
void MC2_ReachableByBearing (const Stuff::Vector3D& c, float out[8])
{
	for (long b = 0; b < 8; b++)
		out[b] = -1.0f;
	if (!GameMap || !land)
		return;

	long row0 = 0, col0 = 0;
	land->worldToCell(c, row0, col0);

	const long R = 26;
	const long SIDE = R * 2 + 1;
	static unsigned char seen[53 * 53];
	memset(seen, 0, sizeof(seen));
	static short queue[53 * 53 * 2];
	long head = 0, tail = 0;

	for (long i = 0; i < SIDE; i++)
	{
		const long edge[4][2] = { {0,i}, {SIDE-1,i}, {i,0}, {i,SIDE-1} };
		for (long e = 0; e < 4; e++)
		{
			const long lr = edge[e][0], lc = edge[e][1];
			if (seen[lr * SIDE + lc])
				continue;
			const long r = row0 - R + lr, cc = col0 - R + lc;
			if (!GameMap->inBounds(r, cc) || !GameMap->getPassable(r, cc))
				continue;
			seen[lr * SIDE + lc] = 1;
			queue[tail * 2] = (short)lr; queue[tail * 2 + 1] = (short)lc; tail++;
		}
	}

	while (head < tail)
	{
		const long lr = queue[head * 2], lc = queue[head * 2 + 1];
		head++;
		const long r = row0 - R + lr, cc = col0 - R + lc;
		Stuff::Vector3D w;
		land->cellToWorld(r, cc, w);
		const float dx = w.x - c.x, dy = w.y - c.y;
		const float dist = sqrtf(dx * dx + dy * dy) * metersPerWorldUnit;
		if ((dx != 0.0f) || (dy != 0.0f))
		{
			float a = atan2f(dy, dx);
			if (a < 0.0f)
				a += 2.0f * 3.14159265f;
			long oct = (long)((a + (3.14159265f / 8.0f)) / (3.14159265f / 4.0f)) % 8;
			if ((out[oct] < 0.0f) || (dist < out[oct]))
				out[oct] = dist;
		}
		for (long dr = -1; dr <= 1; dr++)
			for (long dc = -1; dc <= 1; dc++)
			{
				if (!dr && !dc)
					continue;
				const long nr = lr + dr, nc = lc + dc;
				if ((nr < 0) || (nc < 0) || (nr >= SIDE) || (nc >= SIDE))
					continue;
				if (seen[nr * SIDE + nc])
					continue;
				const long ar = row0 - R + nr, ac = col0 - R + nc;
				if (!GameMap->inBounds(ar, ac) || !GameMap->getPassable(ar, ac))
					continue;
				seen[nr * SIDE + nc] = 1;
				queue[tail * 2] = (short)nr; queue[tail * 2 + 1] = (short)nc; tail++;
			}
	}
}

// M8-B66. AN EXPERIMENT THE MACHINE CAN RUN. The owner reports the prison
// captures from one position and not from others; two theories about WHY died
// on measurement (the gate distance is fine, and the nearest reachable ground
// is 17m against a 30m gate). So stop theorising and make the game try it:
// park a friendly mech on each bearing around the building in turn and issue a
// real capture order. The existing [capture-arrive] trace then prints the
// decision the engine actually made, from each side, in one run.
//
// MC2_CAPTURE_PROBE="x,y" names the building to test. Ticks drive it; one
// bearing per cycle.
void MC2_CaptureProbeTick (void)
{
	static int armed = -1;
	static float wantX = 0.0f, wantY = 0.0f;
	if (armed < 0)
	{
		armed = 0;
		const char* want = getenv("MC2_CAPTURE_PROBE");
		if (want && want[0] && sscanf(want, "%f,%f", &wantX, &wantY) == 2)
			armed = 1;
	}
	if (!armed || !ObjectManager || !Team::home || !Commander::home)
		return;

	static long tick = 0;
	static long bearing = 0;
	const long CYCLE = 150;
	tick++;
	if ((tick % CYCLE) != 1)
		return;
	if (bearing >= 8)
		return;
	// MC2_CAPTURE_PROBE_BEARING=<deg> tests ONE side per run. The building stays
	// captured after the first success, so a sweep can only ever prove the side
	// that happened to go first.
	{
		const char* only = getenv("MC2_CAPTURE_PROBE_BEARING");
		if (only && only[0])
		{
			const long wantDeg = atol(only);
			while ((bearing < 8) && ((bearing * 360 / 8) != wantDeg))
				bearing++;
			if (bearing >= 8)
				return;
		}
	}

	// The building under test.
	BuildingPtr pB = NULL;
	float bestD = 1e9f;
	for (long i = 0; i < ObjectManager->getNumBuildings(); i++)
	{
		BuildingPtr b = ObjectManager->getBuilding(i);
		if (!b || !b->getFlag(OBJECT_FLAG_CAPTURABLE))
			continue;
		const float dx = b->getPosition().x - wantX;
		const float dy = b->getPosition().y - wantY;
		const float dd = dx * dx + dy * dy;
		if (dd < bestD) { bestD = dd; pB = b; }
	}
	if (!pB)
		return;

	// A mech of ours to drive.
	MoverPtr pMover = NULL;
	for (long i = 0; i < ObjectManager->getNumMovers(); i++)
	{
		MoverPtr m = ObjectManager->getMover(i);
		if (!m || !m->isOnGUI() || m->isDestroyed())
			continue;
		if (m->getCommanderId() != Commander::home->getId())
			continue;
		if (m->getObjectClass() != BATTLEMECH)
			continue;
		pMover = m;
		break;
	}
	if (!pMover)
		return;

	// MC2_CAPTURE_PROBE_CLEAR=<metres> (1 = 90) destroys non-player movers that
	// close to the building first. With guards alive a capture is vetoed, which
	// is retail and correct but hides whether the mech walks in at all.
	{
		static bool cleared = false;
		const char* clear = getenv("MC2_CAPTURE_PROBE_CLEAR");
		if (!cleared && clear && clear[0] && strcmp(clear, "0"))
		{
			cleared = true;
			float radius = (float)atof(clear);
			if (radius <= 1.0f)
				radius = 90.0f;
			long killed = 0;
			for (long i = 0; i < ObjectManager->getNumMovers(); i++)
			{
				MoverPtr m = ObjectManager->getMover(i);
				if (!m || m->getCommanderId() == Commander::home->getId())
					continue;
				if (pB->distanceFrom(m->getPosition()) > radius)
					continue;
				m->setStatus(OBJECT_STATUS_DESTROYED);
				killed++;
			}
			printf("[capture-probe] cleared %ld unit(s) within %.0fm of the building\n", killed, radius);
		}
	}

	const Stuff::Vector3D c = pB->getPosition();
	const float ang = (float)bearing * 2.0f * 3.14159265f / 8.0f;

	// Stand where a mech could ACTUALLY stand on this bearing -- the nearest cell
	// reachable from OUTSIDE. The first version of this probe marched a straight
	// line and teleported the mech INSIDE the fence, which is the same mistake
	// the capture gate itself was making, so the probe agreed with the bug.
	float reach[8];
	extern void MC2_ReachableByBearing (const Stuff::Vector3D& cc, float out[8]);
	MC2_ReachableByBearing(c, reach);
	if (reach[bearing] < 0.0f)
	{
		printf("[capture-probe] bearing %3ld deg  UNREACHABLE from outside\n",
			bearing * 360 / 8);
		bearing++;
		return;
	}
	// MC2_CAPTURE_PROBE_EXTRA=<metres> parks that much FURTHER out, so the
	// order has to WALK the mech in (owner 2026-10-01: a capture ordered from
	// 55 m+ left the mech standing still).
	float extra = 0.0f;
	{
		const char* e = getenv("MC2_CAPTURE_PROBE_EXTRA");
		if (e && e[0])
			extra = (float)atof(e);
	}
	Stuff::Vector3D at;
	at.x = c.x + cosf(ang) * ((reach[bearing] + extra) / metersPerWorldUnit);
	at.y = c.y + sinf(ang) * ((reach[bearing] + extra) / metersPerWorldUnit);
	at.z = 0.0f;
	at.z = land->getTerrainElevation(at);

	pMover->setPosition(at);

	const float dx = at.x - c.x, dy = at.y - c.y;
	printf("[capture-probe] bearing %3ld deg  mech at (%.0f,%.0f)  %.2fm from the "
		   "building  buildingTeam=%ld homeTeam=%ld\n",
		bearing * 360 / 8, at.x, at.y,
		sqrtf(dx * dx + dy * dy) * metersPerWorldUnit,
		(long)pB->getTeamId(), (long)Team::home->getId());
	fflush(stdout);

	TacticalOrder tacOrder;
	tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_CAPTURE);
	tacOrder.targetWID = pB->getWatchID();
	tacOrder.attackParams.type = ATTACK_NONE;
	tacOrder.attackParams.method = ATTACKMETHOD_RAMMING;
	tacOrder.attackParams.pursue = true;
	tacOrder.moveParams.wayPath.mode[0] = TRAVEL_MODE_FAST;
	pMover->handleTacticalOrder(tacOrder);

	bearing++;
}

void MC2_MissionProbe (void)
{
	const char* want = getenv("MC2_MISSION_PROBE");
	if (!want || !want[0] || !strcmp(want, "0"))
		return;

	const float kGateMetres = 30.0f;

	// ---- M8-B66: per-bearing reachability around every capturable building.
	if (ObjectManager && GameMap && land)
	{
		for (long i = 0; i < ObjectManager->getNumBuildings(); i++)
		{
			BuildingPtr pB = ObjectManager->getBuilding(i);
			if (!pB || !pB->getFlag(OBJECT_FLAG_CAPTURABLE) || pB->isDestroyed())
				continue;

			Stuff::Vector3D c = pB->getPosition();
			printf("[probe] capturable building %ld at (%.1f,%.1f) -- gate is %.1fm"
				   " (%.0f world units)\n",
				i, c.x, c.y, kGateMetres, kGateMetres / metersPerWorldUnit);

			long inside = 0, outside = 0;
			for (long b = 0; b < 16; b++)
			{
				const float ang = (float)b * 2.0f * 3.14159265f / 16.0f;
				float found = -1.0f;
				for (float r = 8.0f; r <= 600.0f; r += 4.0f)
				{
					Stuff::Vector3D probe;
					probe.x = c.x + cosf(ang) * r;
					probe.y = c.y + sinf(ang) * r;
					probe.z = 0.0f;
					long row = 0, col = 0;
					land->worldToCell(probe, row, col);
					if (!GameMap->inBounds(row, col))
						break;
					if (GameMap->getPassable(row, col))
					{
						found = r;
						break;
					}
				}
				if (found < 0.0f)
				{
					printf("[probe]   bearing %3ld deg  NO passable cell within 600\n",
						(long)(b * 360 / 16));
					outside++;
					continue;
				}
				const float metres = found * metersPerWorldUnit;
				const bool ok = (metres < kGateMetres);
				if (ok) inside++; else outside++;
				printf("[probe]   bearing %3ld deg  nearest standable %6.1f wu = %5.2fm  %s\n",
					(long)(b * 360 / 16), found, metres, ok ? "inside gate" : "OUTSIDE GATE");
			}
			printf("[probe] building %ld: %ld bearing(s) inside the gate, %ld outside\n",
				i, inside, outside);
			{
				float reach[8];
				MC2_ReachableByBearing(c, reach);
				for (long b = 0; b < 8; b++)
					printf("[probe]   bearing %3ld deg  REACHABLE from outside %6.2fm  %s\n",
						b * 45, reach[b],
						(reach[b] < 0.0f) ? "UNREACHABLE"
							: ((reach[b] < kGateMetres) ? "inside gate" : "OUTSIDE GATE"));
			}
		}
	}

	// ---- M8-B65: stand a friendly mover on the first objective's target.
	if (Team::home && ObjectManager)
	{
		CObjective* pFirst = NULL;
		for (EList<CObjective*, CObjective*>::EIterator iter = Team::home->objectives.Begin();
			 !iter.IsDone(); iter++)
		{
			if (*iter && !(*iter)->IsHiddenTrigger() && (*iter)->DisplayMarker())
			{
				pFirst = *iter;
				break;
			}
		}
		if (pFirst)
		{
			for (long i = 0; i < ObjectManager->getNumMovers(); i++)
			{
				MoverPtr pMover = ObjectManager->getMover(i);
				if (!pMover || !pMover->isOnGUI())
					continue;
				if (!Commander::home || (pMover->getCommanderId() != Commander::home->getId()))
					continue;
				Stuff::Vector3D goTo;
				goTo.x = pFirst->MarkerX();
				goTo.y = pFirst->MarkerY();
				goTo.z = 0.0f;
				goTo.z = land->getTerrainElevation(goTo);
				printf("[probe] teleporting mover %ld onto objective 1's target (%.1f,%.1f)"
					   " to reproduce arriving at the dropship\n",
					i, goTo.x, goTo.y);
				pMover->setPosition(goTo);
				break;
			}
		}
	}
}
