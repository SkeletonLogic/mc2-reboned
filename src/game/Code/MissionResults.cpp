#define MISSIONRESULTS_CPP
/*************************************************************************************************\
MissionResults.cpp			: Implementation of the MissionResults component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include <cstdio>
#include "MissionResults.h"
#include "mclib.h" // PORT: match case-sensitive host path.
#include "objmgr.h" // PORT: match case-sensitive host path.
#include "mech.h" // PORT: match case-sensitive host path.
#include "LogisticsVariant.h"
#include "MechIcon.h"
#include "LogisticsData.h"
#include "SalvageMechArea.h"
#include "PilotReviewArea.h"
#include "gamesound.h"
#include "../resource.h" // PORT: use host-independent include separators.

bool MissionResults::FirstTimeResults = true;
MissionResults::MissionResults()
{
	pSalvageScreen = 0;
	pPilotScreen = 0;
	bDone = 0;
	bPilotStarted = 0;
}

MissionResults::~MissionResults()
{
	if (pSalvageScreen)
		delete pSalvageScreen;
	pSalvageScreen = NULL;

	if (pPilotScreen)
		delete pPilotScreen;
	pPilotScreen = NULL;
}

void MissionResults::begin()
{

	if ( soundSystem )
		soundSystem->stopSupportSample("MissionResults");


	if ( !MPlayer )
	{
		char path[256];
		FitIniFile file;
		const bool skipSalvage = LogisticsData::instance->skipSalvageScreen();
		const bool skipPilotReview = LogisticsData::instance->skipPilotReview();
#if defined(__EMSCRIPTEN__)
		std::printf("[browser-results] begin skipSalvage=%d skipPilotReview=%d lastMission=\"%s\"\n",
			static_cast<int>(skipSalvage), static_cast<int>(skipPilotReview),
			LogisticsData::instance->getLastMission().Data());
#endif

		if ( !skipSalvage )
		{
			pSalvageScreen = new SalvageMechScreen();
			strcpy( path, artPath );
			strcat( path, "mcui_mr_layout.fit" );
			
			if ( NO_ERR != file.open( path ) )
			{
				char error[256];
				sprintf( error, "couldn't open file %s", path );
				Assert( 0, 0, error );
				return;
			}
			pSalvageScreen->init( &file );
			file.close();	
			bDone = false;
#if defined(__EMSCRIPTEN__)
			std::printf("[browser-results] salvage screen created\n");
#endif
		}
			
		if ( !skipPilotReview )
		{
		
			strcpy( path, artPath );
			strcat( path, "mcui_mr_layout.fit" );

			pPilotScreen = new PilotReviewScreen();
			if ( NO_ERR != file.open( path ) )
			{
				char error[256];
				sprintf( error, "couldn't open file %s", path );
				Assert( 0, 0, error );
				return;
			}
			pPilotScreen->init( &file );
			bDone = 0;
			if ( !skipSalvage )
				bPilotStarted = 0;
			else
				bPilotStarted = 1;
#if defined(__EMSCRIPTEN__)
			std::printf("[browser-results] pilot screen created pilotStarted=%d\n", static_cast<int>(bPilotStarted));
#endif
		}
	}
	else
	{
		if ( !mpStats.staticCount )
		{
			mpStats.init();

		}

		if ( mpStats.getStatus() != LogisticsScreen::RUNNING )
			mpStats.begin();

		bDone = false;
	}
}

void MissionResults::init()
{

}

void MissionResults::end()
{
	//Need to save the game here so salvage and pilot promotion get saved as well!
	// Pity we never call ::end!
	const char* pMissionName = LogisticsData::instance->getLastMission();

	if ( pMissionName && !LogisticsData::instance->isSingleMission() && !LogisticsData::instance->campaignOver() && !MPlayer )
	{
		FitIniFile file;
		char name[256];
		cLoadString( IDS_AUTOSAVE_NAME, name, 255 );
		char fullName[256];
		sprintf( fullName, name, pMissionName );
		FullPathFileName path;
		path.init( savePath, fullName, ".fit" );
		long createResult = file.create( path );
#if defined(__EMSCRIPTEN__)
		std::printf("[browser-save] autosave create result=%ld path=\"%s\"\n", createResult, static_cast<char*>(path));
#endif
		if ( NO_ERR == createResult )
		{
			long saveResult = LogisticsData::instance->save( file );
#if defined(__EMSCRIPTEN__)
			std::printf("[browser-save] autosave LogisticsData::save result=%ld path=\"%s\"\n", saveResult, static_cast<char*>(path));
#endif
		}
	}
}

void MissionResults::update()
{
	userInput->setMouseCursor( mState_NORMAL );
	userInput->mouseOn();

	if ( MPlayer )
	{
		mpStats.update();
		if ( mpStats.getStatus() != LogisticsScreen::RUNNING )
			bDone = true;
	}

	else if ( !pSalvageScreen && !pPilotScreen )
		bDone = true;

	if ( pSalvageScreen )
	{
		pSalvageScreen->update();
		if ( pSalvageScreen->donePressed() )
		{
			bPilotStarted = true;		
		}

		if ( pSalvageScreen->isDone() )
		{
			pSalvageScreen->updateSalvage();
			delete pSalvageScreen;
			pSalvageScreen = NULL;

			if ( !pPilotScreen )
			{
				bDone = true;
			}
		}

	}
	if ( pPilotScreen && bPilotStarted)
	{
		pPilotScreen->update();
		if ( pPilotScreen->isDone() )
		{
			pPilotScreen->updatePilots();
			delete pPilotScreen;
			pPilotScreen = 0;
			// take this out!
		 
			bDone = true;



			//Also should stop the support sample here. 
			soundSystem->stopSupportSample("MissionResults");
		}
	}

//	if (!soundSystem->isDigitalMusicPlaying())
	{
		//We're done with the win or lose tune.
		// Start the mission results tune as a loop.
		soundSystem->playDigitalMusic(RESULTS_LOOP);
	}
}

void MissionResults::render()
{

	if ( MPlayer )
	{
		mpStats.render(0, 0);
	}

	else if ( pSalvageScreen )
	{
		pSalvageScreen->render();

		//Tutorial -- ONLY do first time we get into the salvage screen.
		if (FirstTimeResults)
		{
			soundSystem->playSupportSample(-1, "tut_4a");
			FirstTimeResults = false;
		}
	}

	if ( pPilotScreen && bPilotStarted )
		pPilotScreen->render();
}

void MissionResults::setHostLeftDlg( const char* pName )
{
	if ( MPlayer && mpStats.getStatus() == LogisticsScreen::RUNNING )
	{
		mpStats.setHostLeftDlg( pName );
	}
}
