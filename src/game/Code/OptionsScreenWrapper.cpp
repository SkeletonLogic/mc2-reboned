#define OPTIONSSCREENWRAPPER_CPP
/*************************************************************************************************\
OptionsScreenWrapper.cpp			: Implementation of the OptionsScreenWrapper component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include "OptionsScreenWrapper.h"
#include "mclib.h" // PORT: match case-sensitive host path.
#include "OptionsArea.h"
#include "mission.h" // PORT: mission options need scoped canvas handling.
#include "mc2_port.h" // PORT: scope fixed-layout options canvas during tactical play.


OptionsScreenWrapper::OptionsScreenWrapper()
{
	pOptionsScreen = 0;
	isActive = false;
	usingMissionCanvas = false;
}

OptionsScreenWrapper::~OptionsScreenWrapper()
{
	if (pOptionsScreen && isActive)
	{
		end();
	}
}

void OptionsScreenWrapper::begin()
{
	// PORT: the options FITs are authored at 800x600. Logistics/main-menu
	// callers already have that virtual canvas active; tactical options are a
	// full-screen overlay, so activate it only for this wrapper lifetime.
	usingMissionCanvas = mission && mission->missionInterface && !MC2_IsVirtualCanvasActive();
	if (usingMissionCanvas)
		MC2_SetVirtualCanvas(800, 600);

	isActive = true;
	if (!pOptionsScreen)
	{
 		pOptionsScreen = new OptionsXScreen();
		char path[256];
		strcpy( path, artPath );
		strcat( path, "mcl_options.fit" );
		
		FitIniFile file;
		if ( NO_ERR != file.open( path ) )
		{
			char error[256];
			sprintf( error, "couldn't open file %s", path );
			Assert( 0, 0, error );
			return;
		}
		pOptionsScreen->init( &file );
	}
}

void OptionsScreenWrapper::init()
{

}

void OptionsScreenWrapper::destroy()
{
}

void OptionsScreenWrapper::end()
{
	delete pOptionsScreen;
	pOptionsScreen = NULL;
	isActive = false;
	if (usingMissionCanvas)
	{
		MC2_SetVirtualCanvas(0, 0);
		usingMissionCanvas = false;
	}
}

OptionsScreenWrapper::status_type OptionsScreenWrapper::update()
{
	if ( pOptionsScreen && isActive)

	{
		userInput->setMouseCursor( mState_NORMAL );
		pOptionsScreen->update();
		if ( pOptionsScreen->isDone() )
		{
			pOptionsScreen->updateOptions();
			isActive = 0;
			return opt_DONE;
		}
		else
		{
			return opt_ACTIVE;
		}
	}
	else
	{
		return opt_INACTIVE;
	}
}

void OptionsScreenWrapper::render()
{
	if ( pOptionsScreen && isActive )
		pOptionsScreen->render();
}

