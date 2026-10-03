#define MISSIONSELECTIONSCREEN_CPP
/*************************************************************************************************\
MissionSelectionScreen.cpp			: Implementation of the MissionSelectionScreen component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include "MissionSelectionScreen.h"
#include "MechBayScreen.h"
#include "LogisticsData.h"
#include "../resource.h" // PORT: use host-independent include separators.
#include "MissionBriefingScreen.h"
#include "Sounds.h" // PORT: match case-sensitive host path.
#include "mc2movie.h"
#include "gamesound.h"

#define VIDEO_RECT 7
#define MAP_RECT 3
#define MSG_FIRST_MISSION 100
#define LB_RECT 4
#define RP_TEXT 4

MissionSelectionScreen::MissionSelectionScreen(  )
{
	status = RUNNING;
	LogisticsScreen::helpTextArrayID = 3;
	bMovie = 0;
	bMovieName[0] = 0;

	missionCount = 0;

	memset( missionNames, 0, sizeof( const char* ) * MAX_MISSIONS_IN_GROUP );
	bStop = 0;

	missionDescriptionListBox.setPressFX( -1 );
	missionDescriptionListBox.setHighlightFX( -1 );
	missionDescriptionListBox.setDisabledFX( -1 );
}

MissionSelectionScreen::~MissionSelectionScreen()
{
	if (bMovie)
	{
		delete bMovie;
		bMovie = NULL;
	}

	missionDescriptionListBox.destroy();
}

//-------------------------------------------------------------------------------------------------


void MissionSelectionScreen::init( FitIniFile* file )
{
	int i;
	LogisticsScreen::init( *file, "CMStatic", "CMTextEntry", "CMRect", "CMButton" );
	for ( i= 0; i < buttonCount; i++ )
		buttons[i].setMessageOnRelease();

	missionCount = 0;

	missionDescriptionListBox.init( rects[LB_RECT].left(), rects[LB_RECT].top(),
		rects[LB_RECT].width(), rects[LB_RECT].height() );

	missionDescriptionListBox.setOrange(true);

	getButton( MN_MSG_PLAY )->setPressFX( LOG_VIDEOBUTTONS );
	getButton( MN_MSG_STOP )->setPressFX( LOG_VIDEOBUTTONS );
	getButton( MN_MSG_PAUSE )->setPressFX( LOG_VIDEOBUTTONS );
	getButton( MN_MSG_PLAY )->setHighlightFX( LOG_DIGITALHIGHLIGHT );
	getButton( MN_MSG_STOP )->setHighlightFX( LOG_DIGITALHIGHLIGHT );
	getButton( MN_MSG_PAUSE )->setHighlightFX( LOG_DIGITALHIGHLIGHT );


}

void MissionSelectionScreen::render(int xOffset, int yOffset )
{
	if ( xOffset == 0 && yOffset == 0 )
		missionDescriptionListBox.render();

	//Renders the movie the old way.
	//movie Now!
	LogisticsScreen::render( xOffset, yOffset );
	if ( !xOffset && !yOffset )
	{
		/*
		gos_VERTEX v[4];

		for( int i = 0; i < 4; i++ )
		{
			v[i].argb = 0xffffffff;
			v[i].frgb = 0;
			v[i].rhw = .5;
			v[i].u = 0.f;
			v[i].v = 0.f;
			v[i].x = rects[VIDEO_RECT].left()+1 + xOffset;
			v[i].y = rects[VIDEO_RECT].top()+1 + yOffset;
			v[i].z = 0.f;
		}

		v[2].x = v[3].x = v[0].x + rects[VIDEO_RECT].width()-2;
		v[2].y = v[1].y = v[0].y + rects[VIDEO_RECT].height()-2;

		v[2].u = v[3].u = 1.0f;
		v[2].v = v[1].v = 1.0f;

		gos_SetRenderState( gos_State_Texture,  videoTexture );
		gos_DrawQuads( v, 4 );
		gos_SetRenderState( gos_State_Texture,  0 );
		*/

		if (bMovie)
			bMovie->render();
	}


	operationScreen.render( xOffset, yOffset );


}

void MissionSelectionScreen::update()
{
	int i;
	if (!playedLogisticsTune)
	{
		soundSystem->playDigitalMusic(LogisticsData::instance->getCurrentMissionTune());
		playedLogisticsTune = true;
	}

	if ( bStop )
	{
		if ( bMovie )
			bMovie->stop();
		bStop = false;
	}

	if ( bMovie )
		bMovie->update();

	LogisticsScreen::update();
	operationScreen.update();

	long oldButton = -1;
	int highlightButton = -1;

	long mouseX = userInput->getMouseX();
	long mouseY = userInput->getMouseY();

	unsigned long highlightColor = 0;

	for ( i = 0; i < operationScreen.buttonCount; i++ )
	{
		if ( operationScreen.buttons[i].isShowing() )
		{
			if ( operationScreen.buttons[i].pointInside( mouseX, mouseY ) )
			{
				highlightButton = i;
				highlightColor = operationScreen.buttons[i].getColor( );
			}

			if ( operationScreen.buttons[i].isPressed() && i != pressedButton )
			{
				handleMessage( 0, operationScreen.buttons[i].getID() );
				oldButton = pressedButton;
				pressedButton = i;
				break;
			}
		}
	}

	if ( oldButton != -1 )
		operationScreen.buttons[oldButton].press( 0 );

	missionDescriptionListBox.update();

	if ( pressedButton != -1 )
	{
		operationScreen.textObjects[0].setText( LogisticsData::instance->getMissionFriendlyName(
			missionNames[pressedButton] ) );
//		if ( highlightColor )
//			operationScreen.textObjects[0].setColor( highlightColor );
	}
	else
	{
		operationScreen.textObjects[0].setText( "" );
	}

	if ( LogisticsData::instance->skipLogistics() ) // the launch button
	{
		buttons[5].showGUIWindow( 1 );
	}
	else
		buttons[5].showGUIWindow( 0 );

	

}

void MissionSelectionScreen::begin()
{
	int i;
	status = RUNNING;
	playedLogisticsTune = false;

	if ( fadeInTime )
		operationScreen.beginFadeIn( fadeInTime );

	// initialize the operation
	const char* str = LogisticsData::instance->getCurrentOperationFileName();

	FullPathFileName fileName;
	fileName.init( artPath, str, ".fit" );

	FitIniFile file;
	if ( NO_ERR != file.open( fileName ) )
	{
		char errorStr[256];
		sprintf( errorStr, "couldn't open file %s", (char*)fileName );
		Assert( 0, 0, errorStr );
	}
	
	operationScreen.moveTo( 0, 0 );
	operationScreen.init( file, "Static", "Text", "Rect", "Button" );
	
	operationScreen.moveTo( rects[MAP_RECT].left(), 
							rects[MAP_RECT].top() );


	
	str = LogisticsData::instance->getCurrentVideoFileName();
	if ( str && strlen( str ) )
	{
		FullPathFileName videoName;
		videoName.init( moviePath, str, ".bik" );

		// PORT (M8-1): ask for the file that will actually be played. The name
		// built above is the retail ".bik"; the run directory holds 185 ".webm"
		// and no ".bik" at all, so testing the name as built answered "no
		// movie" for every mission and this screen never reached MC2Movie.
		if (MC2Movie::mediaExists(videoName) || fileExistsOnCD(videoName))
		{
			RECT movieRect;
			movieRect.left = rects[VIDEO_RECT].left()+1;
			movieRect.top = rects[VIDEO_RECT].top()+1;
			movieRect.right = movieRect.left + rects[VIDEO_RECT].width()-2;
			movieRect.bottom = movieRect.top + rects[VIDEO_RECT].height()-2;
		
			// PORT (M8-B21): THE MISSION-SELECT VIDEO DID NOT AUTOPLAY.
			//
			// begin() runs TWICE on the way into this screen -- MissionBegin::init
			// calls MissionBegin::begin, which calls this -- and it used to tear
			// the movie down and build a fresh one each time. That is fatal here
			// because playing is one-shot: the FIRST pass starts the video and
			// sets videoShown, and the SECOND pass throws that playing movie away,
			// builds a replacement, sees videoShown already set, and stops it. The
			// owner met this after the Steiner-to-Davion cutscene: the screen came
			// up silent and the video only ran when they clicked Play.
			//
			// Measured in their 3.2-million-frame session log: node_10 reports
			// "audio prepared" and "rect" TWICE with no "audio started" between
			// them, then "restarted" when they hit the button. Two builds, no
			// autoplay.
			//
			// So a re-entry for the SAME video now leaves the existing movie
			// alone. A different video still replaces it, which is what the
			// original teardown was actually for -- the comment says "cause we
			// loaded a savegame or something".
			const bool sameVideoAlreadyBuilt =
				(bMovie != NULL) && (strcmp(bMovieName, (char*)videoName) == 0);

			if (!sameVideoAlreadyBuilt)
			{
			//If there is one already here, cause we loaded a savegame or something,
			// Toss it to prevent leaking from the system Heap!
			if (bMovie)
			{
				bMovie->stop();
				delete bMovie;
				bMovie = NULL;
			}

			strncpy(bMovieName, (char*)videoName, sizeof(bMovieName) - 1);
			bMovieName[sizeof(bMovieName) - 1] = 0;

			bMovie = new MC2Movie;
			// PORT (M8-B5): 3.5x total over retail, owner ruling 2026-08-16 --
			// raised from 2x with the comms videos, to keep the two matched.
			// These play against the logistics music and read as thin at retail
			// level: node_04 peaks at -9.7 dB, well below the game's own
			// samples. The samples carry ~3.1x of that and the channel slider
			// carries the rest; see MC2Movie::applyAudioGain.
			bMovie->init(videoName,movieRect,true,3.5f);

			if (Environment.Renderer == 3)
			{
				//DO NOT show the movies by default in software.
				// It will still work, if they hit play.
				bMovie->stop();
			}

			if ( LogisticsData::instance->getVideoShown() )
				bMovie->stop();

			LogisticsData::instance->setVideoShown( );
			}
		}
	}

	missionCount = MAX_MISSIONS_IN_GROUP;
	int result = LogisticsData::instance->getCurrentMissions( missionNames, missionCount );

	EString selMissionName = LogisticsData::instance->getCurrentMission();
	gosASSERT( result == NO_ERR );

	bool bPressed = 0;
	for ( i = 0; i < missionCount; i++ )
	{
		if ( i > operationScreen.buttonCount )
		{
			Assert( 0, 0, "not enough buttons on the operation screen" );
		}
		else 
		{
			operationScreen.buttons[i].setID( MSG_FIRST_MISSION + i );
			operationScreen.buttons[i].setPressFX( LOG_SELECT );
			if ( !LogisticsData::instance->getMissionAvailable( missionNames[i] ) )
				operationScreen.buttons[i].disable( true );


			else
			{
				operationScreen.buttons[i].disable( false );
				if ( selMissionName.Compare( missionNames[i], 0 ) == 0 )
				{
					operationScreen.buttons[i].press( true );
					handleMessage( 0, MSG_FIRST_MISSION + i );
					bPressed = 1;
					pressedButton = i;

				}
				if ( !bPressed && !selMissionName.Length()  )
				{
					bPressed = 1;
					pressedButton = i;
					operationScreen.buttons[i].press( true );
					handleMessage( 0, MSG_FIRST_MISSION + i );
				}
			}
		}

	}

	for ( ; i < operationScreen.buttonCount; i++ )
	{
		operationScreen.buttons[i].showGUIWindow( 0 );
	}

	bStop = 0;
}

void MissionSelectionScreen::end()
{
	if ( bMovie )
	{
		bMovie->stop();

		delete bMovie;
		bMovie = NULL;
	}

	beginFadeOut( 0 );
}

int MissionSelectionScreen::handleMessage( unsigned long msg, unsigned long who )
{
	if ( who >= MSG_FIRST_MISSION && who < MSG_FIRST_MISSION + MAX_MISSIONS_IN_GROUP )
	{
		setMission( who - MSG_FIRST_MISSION );
	}
	
	switch( who )
	{
	case MB_MSG_NEXT:
		status = NEXT;
		break;

	case MB_MSG_PREV:
		status = PREVIOUS;
		break;

	case MN_MSG_PLAY:
		if ( bMovie )
			bMovie->restart();
		getButton( MN_MSG_STOP )->press( 0 );
		getButton( MN_MSG_PAUSE )->press( 0 );
		break;
	case MN_MSG_STOP:
		if ( bMovie )
			bMovie->stop();
		bStop = true;
		break;

	case MN_MSG_PAUSE:
		if ( !getButton( who )->isPressed() )
		{
			if ( bMovie )
				bMovie->pause(0);
			getButton( who )->press(false);
		}
		else
		{
			if ( bMovie )
				bMovie->pause(1);
			getButton( who )->press(true);
		}


		break;

	case MB_MSG_MAINMENU:
		status = MAINMENU;
		break;
	}

	return 0;

}

void MissionSelectionScreen::setMission( int whichOne )
{
	LogisticsData::instance->setCurrentMission( missionNames[whichOne] );

	char text[64];
	sprintf( text, "%ld ", LogisticsData::instance->getCBills() );
	textObjects[RP_TEXT].setText( text );


	updateListBox(); 

}

void MissionSelectionScreen::updateListBox()
{
	missionDescriptionListBox.removeAllItems( true );

	
	aTextListItem* pEntry = new aTextListItem( IDS_MN_LB_FONT );
	pEntry->resize( missionDescriptionListBox.width() - missionDescriptionListBox.getScrollBarWidth() - 2,
		pEntry->height() );
	pEntry->setText( IDS_MN_DIVIDER );
	pEntry->setColor( 0xffC66600 );
	missionDescriptionListBox.AddItem( pEntry );


	pEntry = new aTextListItem( IDS_MN_LB_FONT );
	pEntry->resize( missionDescriptionListBox.width() - missionDescriptionListBox.getScrollBarWidth() - 2,
		pEntry->height() );
	pEntry->setText( LogisticsData::instance->getCurrentMissionFriendlyName() );
	pEntry->setColor( 0xffC66600 );
	missionDescriptionListBox.AddItem( pEntry );

	pEntry = new aTextListItem( IDS_MN_LB_FONT );
	pEntry->resize( missionDescriptionListBox.width() - missionDescriptionListBox.getScrollBarWidth() - 2,
		pEntry->height() );
	pEntry->setText( IDS_MN_DIVIDER );
	pEntry->setColor( 0xffC66600   );
	missionDescriptionListBox.AddItem( pEntry );

	pEntry = new aTextListItem( IDS_MN_LB_FONT );
	pEntry->resize( missionDescriptionListBox.width() - missionDescriptionListBox.getScrollBarWidth() - 2,
		pEntry->height() );
	pEntry->setText( LogisticsData::instance->getCurrentMissionDescription() );
	pEntry->setColor( 0xffC66600 );
	pEntry->sizeToText();
	missionDescriptionListBox.AddItem( pEntry );







}


//*************************************************************************************************
// end of file ( MissionSelectionScreen.cpp )
