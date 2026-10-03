#define OPTIONSAREA_CPP
/*************************************************************************************************\
OptionsArea.cpp			: Implementation of the OptionsArea component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include "OptionsArea.h"
#include "prefs.h"
#include "Inifile.h" // PORT: match case-sensitive host path.
#include "../MCLib/userInput.h" // PORT: match case-sensitive host path.
#include "../resource.h" // PORT: use host-independent include separators.
#include "prefs.h"
#include "missiongui.h" // PORT: match case-sensitive host path.
#include "LogisticsDialog.h"
#include "gamesound.h" // PORT: match case-sensitive host path.
#include "LoadScreen.h"

#ifndef GAMESOUND_H
#include "gamesound.h"
#endif

static bool bShadows = true;
static bool bDetailTexture = true;
static int objectDetail = 0;
static int difficulty = 0;
static bool bUnLimitedAmmo = false;
static long DigitalMasterVolume = 255;
static long MusicVolume = 64;
static long sfxVolume = 64;
static long RadioVolume = 64;
static long BettyVolume = 64;
CPrefs prefs;
CPrefs originalSettings;

#include "mission.h"

extern SoundSystem *sndSystem;

#define MSB_TAB0 200
#define MSB_TAB1 201
#define MSB_TAB2 202
#define MSB_TAB3 203

#define MSG_NEAR 101
#define MSG_FAR	102
#define MSG_TAB	312

#define MSG_TERRAIN_DETAIL	301
#define MSG_PILOT_VIDS	302
#define MSG_OBJECT_DETAIL	303
#define MSG_SHADOWS		304
#define MSG_NON_WEAPON	305
#define MSG_LOCAL_SHADOWS	306
#define MSG_ASYNC_MOUSE		307
#define MSG_HARDWARE_RASTERIZER 308
#define MSG_RESET	309
#define MSG_FOG		310

// PORT: authored graphics-options art leaves this row open between the
// checkboxes and resolution controls. Keep the new setting in that space so no
// retail .fit or texture asset must be modified or redistributed.
static const long RENDER_DISTANCE_TRACK_LEFT = 180;
static const long RENDER_DISTANCE_TRACK_TOP = 142;
static const long RENDER_DISTANCE_TRACK_WIDTH = 190;
static const long RENDER_DISTANCE_TRACK_HEIGHT = 12;
static const long FOG_BUTTON_LEFT = 450;
static const long FOG_BUTTON_TOP = 142;

#define MSG_GREEN	400
#define MSG_REGULAR	401
#define MSG_VETERAN	402
#define MSG_ELITE	403

#define MSG_BASE	404
#define MSG_ACCENT	405

#define MSG_UNLIMITED_AMMO	406
#define MSG_LEFT_CLICK		407
#define MSG_SAVE_TRANSCRIPT	408
#define MSG_TUTORIALS		409

#define CTRL	0x10000000
#define SHIFT	0x01000000
#define ALT		0x00100000
#define WAYPT	0x20000000

HotKeyListItem*		HotKeyListItem::s_item = NULL;


OptionsXScreen::OptionsXScreen()
{
	statics = 0;
	rects = 0;
	staticCount = rectCount = buttonCount = textCount = 0;
	buttons = 0;
	textObjects = 0;
	bDone = 0;
	curTab = 0;
	helpTextArrayID = 1;
}

OptionsXScreen::~OptionsXScreen()
{
	int i;
	for ( i = 0; i < 4; i++ )
		delete tabAreas[i];
}

int OptionsXScreen::indexOfButtonWithID(int id)
{
	int i;
	for (i = 0; i < buttonCount; i++)
	{
		if (buttons[i].getID() == id)
		{
			return i;
		}
	}
	return -1;
}

void OptionsXScreen::init(FitIniFile* file)
{
	int i;

	LogisticsScreen::init( *file, "Static", "Text", "Rect", "Button", "Edit" );	

	const char* fileNames[4] = 
	{
		"mcl_optionsgraphics",
		"mcl_optionsaudio",
		"mcl_optionsgameplay",
		"mcl_optionshotkeys"
	};

	OptionsGraphics* pGraphics = new OptionsGraphics;
	tabAreas[0] = pGraphics;
	OptionsAudio* pAudio = new OptionsAudio;
	tabAreas[1] = pAudio;
	OptionsGamePlay* pPlay = new OptionsGamePlay;
	tabAreas[2] = pPlay;
	OptionsHotKeys* pKeys = new OptionsHotKeys;
	tabAreas[3] = pKeys;

	FullPathFileName path;
	for ( i = 0; i < 4;i++ )
	{
		path.init( artPath, fileNames[i], ".fit" );
		FitIniFile tmpFile;
		if ( NO_ERR != tmpFile.open( path ) )
		{
			char error[256];
			sprintf( error, "couldn't open file %s", (char*)path );
			Assert( 0, 0, error );
			return;	
		}

		tabAreas[i]->init( tmpFile, "Static", "Text", "Rect", "Button", "Edit" );
		tabAreas[i]->moveTo( rects[2].x(), rects[2].y() );

	}

	// 640: offset x = -80, y = -90
	// 800: offset none
	// 1024: offset x = 113, y = 54
	// 1280: offset x = 240, y = 182
	// 1600: offset x = 400, y = 270

	long xOffset = 0;
	long yOffset = 0;

	switch (Environment.screenWidth)
	{
	case 640:
		xOffset = -80;
		yOffset = -90;
		break;

	case 1024:
		xOffset = 13;
		yOffset = 54;
		break;
	
	case 1280:
		xOffset = 240;
		yOffset = 182;
		break;
	
	case 1600:
		xOffset = 400;
		yOffset = 270;
		break;



	}

	move( xOffset, yOffset );

	pGraphics->init(xOffset, yOffset);
	pAudio->init(xOffset, yOffset);
	pPlay->init(xOffset, yOffset);
	pKeys->init(xOffset, yOffset);

	//Needs to be 8.3 or it won't go on the CD!!
	originalSettings.load( "OrgPrefs" );

	prefs.load();


	for ( i = 0; i < 4; i++ )
		tabAreas[i]->begin();

	if ( mission && strlen( mission->getMissionFileName() ) )
	{
		getButton( MSB_TAB2 )->disable( true );
	}

	for ( i = 0; i < buttonCount; i++ )
	{
		if ( MSB_TAB0 > buttons[i].getID() || MSB_TAB3 < buttons[i].getID() )
			buttons[i].setMessageOnRelease( );
	}

	getButton( MSB_TAB0 )->press( true );

	bShowWarning = 0;
}

void OptionsXScreen::render()
{
	GUI_RECT rect = { 0, 0, Environment.screenWidth, Environment.screenHeight };
	drawRect( rect, 0xff000000 );
	rects[1].setColor( 0xff000000 );
	rects[1].render();
	if ( curTab < 2 )
		tabAreas[curTab]->render();
	rects[1].setColor( 0 );
	LogisticsScreen::render();


	getButton( MSB_TAB0 + curTab )->render();

	if ( curTab >= 2 )
		tabAreas[curTab]->render();

	if ( bShowWarning )
	{
		LogisticsOneButtonDialog::instance()->render();
	}
}

int	OptionsXScreen::handleMessage( unsigned long message, unsigned long who)
{
	int i;
	if ( aMSG_LEFTMOUSEDOWN == message )
	{
		switch ( who )
		{

		case MSB_TAB0:
		case MSB_TAB1:
		case MSB_TAB2:
		case MSB_TAB3:
		{
			for ( i = MSB_TAB0; i < MSB_TAB3+1; i++ )
				getButton( i )->press( 0 );
			getButton( who )->press( true );
			curTab = who - MSB_TAB0;
		}
			break;

		case YES:
			{
				int oldRes = prefs.resolution;
				int oldDepth = prefs.bitDepth;
				for ( i = 0; i < 4; i++ )
					tabAreas[i]->end();

				prefs.save();
				prefs.applyPrefs(0);
				LoadScreenWrapper::changeRes();

				int newRes = prefs.resolution;
				int newDepth = prefs.bitDepth;

				if ( newRes != oldRes || newDepth != oldDepth )
				{
					LogisticsOneButtonDialog::instance()->setText( IDS_SWAP_RESOLUTION_WARNING, IDS_DIALOG_OK, IDS_DIALOG_OK );
					LogisticsOneButtonDialog::instance()->begin();
					bShowWarning = true;
				}
				else
					bDone = true;

				return 1;
			}
			break;
		case NO:
			{
				(dynamic_cast<OptionsGamePlay *>(tabAreas[2]))->resetCamera();
				prefs.load();
				prefs.applyPrefs(0);
				bDone = true;
				return 1;
			}
			break;

		}
	}

	return 0;

}

bool OptionsXScreen::isDone()
{
	return bDone;
}

void OptionsXScreen::update()
{

	if ( bShowWarning )
	{
		LogisticsOneButtonDialog::instance()->update();
		if ( LogisticsOneButtonDialog::instance()->isDone() )
			bDone = true;

		return;
	}
		
	else
	{
		LogisticsScreen::update();

		tabAreas[curTab]->update();
	}
	


}

void OptionsXScreen::updateOptions()
{
}


//////////////////////////////////////////////
typedef struct
{
	long xRes;
	long yRes;
	long bitDepth;
	long resourceID;
	long legacyResolution;
} ResModes;

// PORT: preserve every retail mode/label, then add a curated modern desktop
// catalogue. Modern backends are 32-bit, so do not double the added list with
// obsolete 16-bit duplicates; the ten original choices remain byte-for-byte.
ResModes resModes[] = {
	 640,  480, 16, IDS_RESOLUTION0, 0,
	 640,  480, 32, IDS_RESOLUTION1, 0,
	 800,  600, 16, IDS_RESOLUTION2, 1,
	 800,  600, 32, IDS_RESOLUTION3, 1,
	1024,  768, 16, IDS_RESOLUTION4, 2,
	1024,  768, 32, IDS_RESOLUTION5, 2,
	1280, 1024, 16, IDS_RESOLUTION6, 3,
	1280, 1024, 32, IDS_RESOLUTION7, 3,
	1600, 1200, 16, IDS_RESOLUTION8, 4,
	1600, 1200, 32, IDS_RESOLUTION9, 4,
	1152,  864, 32, 0, 5,
	1280,  720, 32, 0, 5,
	1280,  768, 32, 0, 5,
	1280,  800, 32, 0, 5,
	1360,  768, 32, 0, 5,
	1366,  768, 32, 0, 5,
	1440,  900, 32, 0, 5,
	1536,  864, 32, 0, 5,
	1600,  900, 32, 0, 5,
	1680, 1050, 32, 0, 5,
	1920, 1080, 32, 0, 5,
	1920, 1200, 32, 0, 5,
	2048, 1152, 32, 0, 5,
	2560, 1080, 32, 0, 5,
	2560, 1440, 32, 0, 5,
	2560, 1600, 32, 0, 5,
	2880, 1800, 32, 0, 5,
	3200, 1800, 32, 0, 5,
	3440, 1440, 32, 0, 5,
	3840, 1600, 32, 0, 5,
	3840, 2160, 32, 0, 5,
	5120, 1440, 32, 0, 5,
	5120, 2160, 32, 0, 5,
	5120, 2880, 32, 0, 5
};

enum { RES_MODE_COUNT = sizeof(resModes) / sizeof(resModes[0]) }; // PORT: keep iteration tied to the table.
bool availableMode[RES_MODE_COUNT];

void OptionsGraphics::init(long xOffset, long yOffset)
{
	int i;
	FullPathFileName path;
	path.init( artPath, "mcl_options_combobox0", ".fit" );
	FitIniFile file;
	if ( NO_ERR !=file.open( path ) )
	{
		char error[256];
		sprintf( error, "couldn't open file %s", (char*)path );
		Assert( 0, 0, error );
		return;	
	}

	getButton( MSG_RESET )->setMessageOnRelease();

	resolutionList.init( &file, "PlayerNameComboBox" );
	resolutionList.move( globalX(), globalY() );
	resolutionList.move( xOffset, yOffset );
	resolutionList.ListBox().setOrange( true );

	// PORT: retail entries keep their localized strings; added modes use the same
	// "W x H x depth" presentation and remain subject to platform validation.
	for ( i = 0; i < RES_MODE_COUNT; i++ )
	{
		if ( 1!=gos_GetMachineInformation( gos_Info_ValidMode,
			Environment.FullScreenDevice,
			resModes[i].xRes,
			resModes[i].yRes,
			resModes[i].bitDepth) )
		{
			availableMode[i] = false;
		}
		else
		{
			char cstr[256];
			availableMode[i] = true;
			if (resModes[i].resourceID)
				cLoadString (resModes[i].resourceID, cstr, 255 );
			else
				sprintf(cstr, "%ld x %ld x %ld", resModes[i].xRes,
					resModes[i].yRes, resModes[i].bitDepth);
			resolutionList.AddItem( cstr, 0xffffffff );
		}
	}

	file.close();

	// PORT: geometry is port-owned; text reuses the authored heading font in
	// place so no duplicate font texture is allocated.
	renderDistanceDragging = false;

	path.init( artPath, "mcl_options_combobox2", ".fit" );
	if ( NO_ERR !=file.open( path ) )
	{
		char error[256];
		sprintf( error, "couldn't open file %s", (char*)path );
		Assert( 0, 0, error );
		return;	
	}

	getButton( MSG_RESET )->setMessageOnRelease();

	cardList.init( &file, "PlayerNameComboBox" );
	cardList.move( globalX(), globalY() );
	cardList.move( xOffset, yOffset );
	cardList.ListBox().setOrange( true );

	DWORD numDevices = 0;
	numDevices = gos_GetMachineInformation( gos_Info_NumberDevices );

	//Theoretically impossible but config would probably like to know if it happens!
	if (numDevices <= 0)
		STOP(("GameOS said there were no video cards in the system!"));

	long usableCardCount = 0;
	for (i=0;i<numDevices;i++)
	{
		DWORD minTextureRam = 6291456;

		//If we are a Voodoo 2, we may be a 4/8 or a 4/4.  Try allowing a 4/4 to run
		// and see what happens!!  NO good has come of this!
		/*
		if ((gos_GetMachineInformation(gos_Info_GetDeviceVendorID,i) == 0x121a) &&
			(gos_GetMachineInformation(gos_Info_GetDeviceDeviceID,i) == 0x0002))
			minTextureRam = 4096000;
		*/

		if (gos_GetMachineInformation(gos_Info_GetDeviceLocalMemory, i) >= minTextureRam)
		{
			char *deviceName = (char*)gos_GetMachineInformation( gos_Info_GetDeviceName, i);
			// PORT: capability peers may omit a display name; never hand the list a null string.
			if (!deviceName)
				deviceName = "MechCommander 2 Display Adapter";
		
			//Save name to other string here.
			cardList.AddItem( deviceName, 0xffffffff );

			if ( Environment.FullScreenDevice == i )
				cardList.SelectItem( usableCardCount );

			usableCardCount++;
		}
	}

//ALL of this is a lie.  Code is now correct but I would ignore this.
// Sorry Heidi!
// -fs

//Turns out, its the same for both.

//Environment.Renderer tells you what device you are currently using as follows:
//	If the value is 0 or 1 you are using the default device or the primary display adapter (One and the same always)
//	If the value is 2 you are using the second video card.
//	If the value is 3 you are using BLADE software renderer.

//If you set the values to any of the above, you will switch to that device.


	for ( i = 0; i < buttonCount; i++ )
	{
		buttons[i].setPressFX( LOG_VIDEOBUTTONS );
		buttons[i].setHighlightFX( LOG_DIGITALHIGHLIGHT );
		buttons[i].setDisabledFX( LOG_WRONGBUTTON );
	}


	move( xOffset, yOffset );

	// PORT (M8-3): reuse the authored checkbox visuals and localized fog label,
	// but place the new control in the same deliberately open row as the port's
	// render-distance slider. No retail .fit or texture is changed or shipped.
	fogButton = buttons[2]; // shadows checkbox: identical toggle styling
	fogButton.moveTo(FOG_BUTTON_LEFT, FOG_BUTTON_TOP);
	addChild(&fogButton);
	fogButton.setID(MSG_FOG);
	fogButton.setText(IDS_OPTIONS_FOG_DISTANCE); // aButton renders this owner-approved label as "FOG".
	fogButton.setTextRegion(globalX() + FOG_BUTTON_LEFT + 20, globalY() + FOG_BUTTON_TOP - 2,
		globalX() + 599, globalY() + FOG_BUTTON_TOP + 16);
	fogButton.setHelpID(IDS_OPTIONS_HELP_FOG_DISTANCE);

	helpTextArrayID = 1;



	
}

int		OptionsGraphics::handleMessage( unsigned long message, unsigned long fromWho )
{
	if ( fromWho == MSG_RESET )
		reset(originalSettings);

	return 1;
}

void OptionsGraphics::render()
{

	LogisticsScreen::render();

	const GUI_RECT track = renderDistanceTrack();
	drawRect(track, 0xff161a20);
	drawEmptyRect(track, 0xff78818c, 0xff78818c);
	GUI_RECT fill = track;
	fill.right = fill.left + (RENDER_DISTANCE_TRACK_WIDTH * prefs.renderDistancePos) / 100;
	if (fill.right > fill.left)
		drawRect(fill, 0xffff7e00);
	const long thumbX = track.left + (RENDER_DISTANCE_TRACK_WIDTH * prefs.renderDistancePos) / 100;
	GUI_RECT thumb = { thumbX - 2, track.top - 3, thumbX + 3, track.bottom + 3 };
	drawRect(thumb, 0xffffffff);
	textObjects[0].font.render("RENDER DISTANCE", globalX() + 24, globalY() + 137,
		150, 18, textObjects[0].getColor(), 0, textObjects[0].alignment);
	char distanceText[128];
	sprintf(distanceText, "%ld%%", prefs.renderDistancePos);
	textObjects[0].font.render(distanceText, globalX() + 382, globalY() + 137,
		55, 18, textObjects[0].getColor(), 0, textObjects[0].alignment);
	fogButton.render();

	resolutionList.render();
	
	cardList.render();

}

void OptionsGraphics::update()
{

	if ( resolutionList.IsExpanded() )
	{
		textObjects[helpTextArrayID].setText( "" );
		helpTextID = 0;
		resolutionList.update();
		if ( helpTextID )
			textObjects[helpTextArrayID].setText( helpTextID );
		bExpanded = true;
	}
	else
	{
		if ( !bExpanded )
		{
			LogisticsScreen::update();
		}

		if ( userInput->leftMouseReleased() )
			bExpanded = 0;

	}

//	if ( cardList.ListBox().GetItemCount() > 1 )
		cardList.update();

	if ( !mission || !strlen( mission->getMissionFileName() ) )
	{
		if ( !bExpanded )
			resolutionList.update(); // don't want to call 2x's
	}

	else if ( userInput->isLeftClick() &&
		resolutionList.pointInside( userInput->getMouseX(), userInput->getMouseY() ) )
	{
		soundSystem->playDigitalSample( LOG_WRONGBUTTON );
	}

	if (!bExpanded)
	{
		const GUI_RECT track = renderDistanceTrack();
		const long mouseX = userInput->getMouseX();
		const long mouseY = userInput->getMouseY();
		if (userInput->isLeftClick() && mouseX >= track.left && mouseX <= track.right &&
			mouseY >= track.top - 6 && mouseY <= track.bottom + 6)
			renderDistanceDragging = true;
		if (renderDistanceDragging &&
			(userInput->isLeftDrag() || gos_GetKeyStatus(KEY_LMOUSE) == KEY_HELD))
			setRenderDistanceFromMouse(mouseX);
		if (userInput->leftMouseReleased())
		{
			if (renderDistanceDragging)
				setRenderDistanceFromMouse(mouseX);
			renderDistanceDragging = false;
		}
	}
	fogButton.update();
	

}

void OptionsGraphics::begin()
{
	helpTextArrayID = 1;
	reset(prefs);

}
void OptionsGraphics::end()
{
	int i;
	prefs.useWaterInterestTexture = getButton( MSG_TERRAIN_DETAIL )->isPressed(  );

	prefs.pilotVideos = getButton( MSG_PILOT_VIDS )->isPressed();
	prefs.useHighObjectDetail = getButton( MSG_OBJECT_DETAIL )->isPressed();
	prefs.useShadows = getButton( MSG_SHADOWS )->isPressed();
	prefs.useNonWeaponEffects = getButton( MSG_NON_WEAPON )->isPressed();
	prefs.useLocalShadows = getButton( MSG_LOCAL_SHADOWS )->isPressed();
	prefs.asyncMouse = getButton( MSG_ASYNC_MOUSE )->isPressed();
	prefs.useFog = fogButton.isPressed();
	prefs.renderer = getButton( MSG_HARDWARE_RASTERIZER )->isPressed() ? 0 : 3;

	int sel = resolutionList.GetSelectedItem();
	if ( sel > -1 )
	{
		long actualSel = -1;
		// PORT: map the filtered list directly back to its exact mode record.
		for ( i = 0; i < RES_MODE_COUNT; i++ )
		{
			if (availableMode[i])
				actualSel++;

			if (actualSel == sel)
			{
				prefs.resolution = resModes[i].legacyResolution;
				prefs.screenWidth = resModes[i].xRes;
				prefs.screenHeight = resModes[i].yRes;
				prefs.bitDepth = resModes[i].bitDepth == 32 ? 1 : 0;

				break;
			}
		}
	}

	int index = cardList.GetSelectedItem( );
	if ( (index != -1) && (prefs.renderer != 3))
		prefs.renderer = index;

}
void OptionsGraphics::reset(const CPrefs& newPrefs)
{
	
	getButton( MSG_TERRAIN_DETAIL )->press( newPrefs.useWaterInterestTexture );
	getButton( MSG_PILOT_VIDS )->press( newPrefs.pilotVideos );
	getButton( MSG_OBJECT_DETAIL )->press( newPrefs.useHighObjectDetail );
	getButton( MSG_SHADOWS )->press( newPrefs.useShadows );
	getButton( MSG_NON_WEAPON )->press( newPrefs.useNonWeaponEffects );
	getButton( MSG_LOCAL_SHADOWS )->press( newPrefs.useLocalShadows );
	getButton( MSG_ASYNC_MOUSE )->press( newPrefs.asyncMouse );
	getButton( MSG_HARDWARE_RASTERIZER )->press( (newPrefs.renderer != 3) );
	fogButton.press( newPrefs.useFog );
	prefs.renderDistancePos = CPrefs::clampPercent(newPrefs.renderDistancePos);

	// PORT: locate the saved exact mode in the filtered display list.  This keeps
	// selection correct even when the platform rejects an entry before it.
	long displayedIndex = -1;
	long selectedIndex = -1;
	for (long i = 0; i < RES_MODE_COUNT; i++)
	{
		if (!availableMode[i])
			continue;
		displayedIndex++;
		if (resModes[i].xRes == newPrefs.screenWidth &&
			resModes[i].yRes == newPrefs.screenHeight &&
			resModes[i].bitDepth == (newPrefs.bitDepth ? 32 : 16))
		{
			selectedIndex = displayedIndex;
			break;
		}
	}
	// PORT: availableMode lost its all-true initialiser when it grew to
	// RES_MODE_COUNT, so it is zero-initialised until init() scans it. If reset()
	// ever runs first, nothing is available and the list box is empty too --
	// selecting row 0 of an empty list is not a meaningful thing to ask for.
	if (displayedIndex < 0)
		return;

	if (selectedIndex < 0)
		selectedIndex = 0;
	resolutionList.SelectItem(selectedIndex);
}

GUI_RECT OptionsGraphics::renderDistanceTrack() const
{
	GUI_RECT result = {
		globalX() + RENDER_DISTANCE_TRACK_LEFT,
		globalY() + RENDER_DISTANCE_TRACK_TOP,
		globalX() + RENDER_DISTANCE_TRACK_LEFT + RENDER_DISTANCE_TRACK_WIDTH,
		globalY() + RENDER_DISTANCE_TRACK_TOP + RENDER_DISTANCE_TRACK_HEIGHT
	};
	return result;
}

void OptionsGraphics::setRenderDistanceFromMouse(long mouseX)
{
	const GUI_RECT track = renderDistanceTrack();
	const long value = ((mouseX - track.left) * 100) / RENDER_DISTANCE_TRACK_WIDTH;
	prefs.renderDistancePos = CPrefs::clampPercent(value);
}

//*************************************************************************************************

//////////////////////////////////////////////

void OptionsAudio::init(long xOffset, long yOffset)
{
	int i;
	getButton( MSG_RESET )->setMessageOnRelease();

	for ( i = 0; i < buttonCount; i++ )
	{
		if ( buttons[i].getID() != MSG_RESET )
		{
			buttons[i].setParent( NULL );
		}
	}
	move( xOffset, yOffset );

	for ( i = 0; i < buttonCount; i++ )
	{
		buttons[i].setPressFX( LOG_VIDEOBUTTONS );
		buttons[i].setHighlightFX( LOG_DIGITALHIGHLIGHT );
		buttons[i].setDisabledFX( LOG_WRONGBUTTON );

	}

	for ( i = 0; i < 5; i++ )
	{
		scrollBars[i].init( &buttons[i * 2], &buttons[i * 2 + 1], &buttons[i + 11] );
		addChild( &scrollBars[i] );
		scrollBars[i].move( -x(), -y() );
		scrollBars[i].SetScrollMax( 255 );
	}

	helpTextArrayID = 15;
	
}

int		OptionsAudio::handleMessage( unsigned long message, unsigned long fromWho )
{
	if ( fromWho == MSG_RESET )
		reset(originalSettings);

	return 1;
}

void OptionsAudio::render()
{
	int i;
	LogisticsScreen::render();

	for ( i = 0; i < 5; i++ )
	{
		scrollBars[i].render();
	}
}

void OptionsAudio::update()
{
	int i;
	LogisticsScreen::update();
	for ( i = 0; i < 5; i++ )
	{
		scrollBars[i].update();
	}
	
	//Lets update these on the fly so they can hear how much better it sounds.
	prefs.DigitalMasterVolume = scrollBars[0].GetScrollPos();
	prefs.MusicVolume = scrollBars[1].GetScrollPos();
	prefs.sfxVolume = scrollBars[2].GetScrollPos();
	prefs.RadioVolume = scrollBars[3].GetScrollPos();
	prefs.BettyVolume = scrollBars[4].GetScrollPos();

	if (sndSystem) 
	{
		sndSystem->setDigitalMasterVolume(prefs.DigitalMasterVolume);
		sndSystem->setSFXVolume(prefs.sfxVolume);
		sndSystem->setRadioVolume(prefs.RadioVolume);
		sndSystem->setMusicVolume(prefs.MusicVolume);
		sndSystem->setBettyVolume(prefs.BettyVolume);
	}
}

void OptionsAudio::begin()
{
	helpTextArrayID = 15;
	reset(prefs);

}
void OptionsAudio::end()
{
	prefs.DigitalMasterVolume = scrollBars[0].GetScrollPos();
	prefs.MusicVolume = scrollBars[1].GetScrollPos();
	prefs.sfxVolume = scrollBars[2].GetScrollPos();
	prefs.RadioVolume = scrollBars[3].GetScrollPos();
	prefs.BettyVolume = scrollBars[4].GetScrollPos();


}
void OptionsAudio::reset(const CPrefs& newPrefs)
{
	scrollBars[0].SetScrollPos( newPrefs.DigitalMasterVolume );
	scrollBars[1].SetScrollPos( newPrefs.MusicVolume );
	scrollBars[2].SetScrollPos( newPrefs.sfxVolume );
	scrollBars[3].SetScrollPos( newPrefs.RadioVolume );
	scrollBars[4].SetScrollPos( newPrefs.BettyVolume );


}

//////////////////////////////////////////////
void OptionsGamePlay::resetCamera()
{
	//Do NOT setup a camera in mission.
	// A.  We don't need it cause the option is greyed out.
	// B.  It creates a mech which will NOT be deleted until we
	// come back to logistics, at which point, THESE heaps will be gone!!
	// Then, CRASH!
	if ( mission && strlen( mission->getMissionFileName() ) )
	{
	}
	else
	{
		camera.setMech( NULL );
	}
}

void OptionsGamePlay::init(long xOffset, long yOffset)
{
	int i;

	camera.init(rects[1].globalX() + xOffset, rects[1].globalY() + yOffset, 
		rects[1].globalRight() + xOffset, rects[1].globalBottom()+yOffset);

	camera.setInMission();

	getButton( MSG_BASE )->press( true );
	getButton( MSG_ACCENT )->press( 0 );

	for ( i = 0; i < buttonCount; i++ )
	{
		buttons[i].setPressFX( LOG_VIDEOBUTTONS );
		buttons[i].setHighlightFX( LOG_DIGITALHIGHLIGHT );
		buttons[i].setDisabledFX( LOG_WRONGBUTTON );

	}

	move( xOffset, yOffset );

	helpTextArrayID = 2;
	
}

int		OptionsGamePlay::handleMessage( unsigned long message, unsigned long fromWho )
{
	int i;
	if ( fromWho >= MSG_GREEN && fromWho < MSG_ELITE+1 )
	{
		for ( i = MSG_GREEN; i < MSG_ELITE+1; i++ )
		{
			getButton( i )->press( 0 );
		}

		getButton( fromWho )->press( true );
	}

	switch( fromWho )
	{
		case MSG_RESET:
			reset(originalSettings);
			break;

		case MSG_BASE:
		case MSG_ACCENT:
			getButton( MSG_BASE )->press( 0 );
			getButton( MSG_ACCENT )->press( 0 );
			getButton( fromWho )->press( true );

		break;
	}

	return 1;
}

void OptionsGamePlay::render()
{
	int i;

	LogisticsScreen::render();


	long colorToMatch = getButton( MSG_BASE )->isPressed() ? 
		rects[36].getColor() : rects[37].getColor();
	for ( i = 4; i < 36; i++ )
	{
		if ( rects[i].getColor() == colorToMatch )
		{
			GUI_RECT tmp = { rects[i].globalX() - 2,
							rects[i].globalY() - 2,
							rects[i].globalRight() + 1,
							rects[i].globalBottom() + 1 };

			drawEmptyRect( tmp, 0xffffffff, 0xffffffff );
			break;
			
		}
	}

	rects[37].render();
	rects[36].render();

	camera.render();

}

void OptionsGamePlay::update()
{
	int i;
	camera.update();

	LogisticsScreen::update();
	
	aRect* pRect = getButton( MSG_BASE )->isPressed() ? 
		&rects[36] : &rects[37];

	bool bChanged = 0;
	if ( userInput->isLeftClick() )
	{
		for ( i = 4; i < 36; i++ )
		{
			if ( rects[i].pointInside( userInput->getMouseX(),
										userInput->getMouseY() ) )
			{
				pRect->setColor( rects[i].getColor() );
				bChanged = 1;
				break;
			}
		}
	}

	if ( bChanged )
		camera.setMech( "Bushwacker", rects[36].getColor(), rects[37].getColor(), rects[37].getColor() );
}

void OptionsGamePlay::begin()
{
	helpTextArrayID = 2;
	reset(prefs);

	//Do NOT setup a camera in mission.
	// A.  We don't need it cause the option is greyed out.
	// B.  It creates a mech which will NOT be deleted until we
	// come back to logistics, at which point, THESE heaps will be gone!!
	// Then, CRASH!
	if ( mission && strlen( mission->getMissionFileName() ) )
	{
	}
	else
	{
		camera.setMech( "Bushwacker", prefs.baseColor, 
			prefs.highlightColor, prefs.highlightColor );
	}
}

void OptionsGamePlay::end()
{
	int i;
	for ( i  = MSG_GREEN; i < MSG_ELITE + 1; i++ )
	{
		if ( getButton( i )->isPressed() )
			prefs.GameDifficulty = i - MSG_GREEN;
	}

	prefs.baseColor = rects[36].getColor(  );
	prefs.highlightColor = rects[37].getColor( );

	prefs.useUnlimitedAmmo = getButton( MSG_UNLIMITED_AMMO )->isPressed();
	prefs.useLeftRightMouseProfile = getButton( MSG_LEFT_CLICK )->isPressed();
	//prefs.tutorials = getButton( MSG_TUTORIALS )->isPressed();

	//Do NOT setup a camera in mission.
	// A.  We don't need it cause the option is greyed out.
	// B.  It creates a mech which will NOT be deleted until we
	// come back to logistics, at which point, THESE heaps will be gone!!
	// Then, CRASH!
	if ( mission && strlen( mission->getMissionFileName() ) )
	{
	}
	else
	{
		camera.setMech( NULL );
	}

}
void OptionsGamePlay::reset(const CPrefs& newPrefs)
{
	int i;
	for ( i  = MSG_GREEN; i < MSG_ELITE + 1; i++ )
	{
		getButton( i )->press( 0 );
	}

	getButton( MSG_GREEN + newPrefs.GameDifficulty )->press( true );

	rects[36].setColor( newPrefs.baseColor );
	rects[37].setColor( newPrefs.highlightColor );

	getButton( MSG_UNLIMITED_AMMO )->press( newPrefs.useUnlimitedAmmo );
	getButton( MSG_LEFT_CLICK )->press( newPrefs.useLeftRightMouseProfile );
//	getButton( MSG_TUTORIALS )->press( newPrefs.tutorials );
}

//*************************************************************************************************

//////////////////////////////////////////////

void OptionsHotKeys::init(long xOffset, long yOffset)
{


	hotKeyList.init( rects[0].x(), rects[0].y(), rects[0].width(), rects[0].height() );	
	helpTextArrayID = 2;
	HotKeyListItem::init();
	hotKeyList.setOrange(true);
	bShowDlg = 0;

	move( xOffset, yOffset );
	hotKeyList.move( xOffset, yOffset );
	helpTextArrayID = 2;
}

int		OptionsHotKeys::handleMessage( unsigned long message, unsigned long fromWho )
{
	switch( fromWho )
	{
		case MSG_RESET:
			reset(true);
			break;
	}

	return 1;
}

void OptionsHotKeys::render()
{

	hotKeyList.render();
	LogisticsScreen::render();

	if ( bShowDlg )
	{
		LogisticsOKDialog::instance()->render();
	}
}

void OptionsHotKeys::update()
{
	int i;

	if ( bShowDlg )
	{
		LogisticsOKDialog::instance()->update();
		if ( LogisticsOKDialog::instance()->isDone() )
		{
			bShowDlg = 0;
			if ( LogisticsDialog::YES == LogisticsOKDialog::instance()->getStatus() )
			{
				char keysString[256];
				keysString[0] = 0;

				makeKeyString( curHotKey, keysString );

				
				long index = hotKeyList.GetSelectedItem();
				long oldKey = -1;
				
				if ( index > -1 )
				{
					HotKeyListItem* pItemToSet = (HotKeyListItem*)hotKeyList.GetItem( index );
					// now I've got to find the other one with th new key and set it to the old key
					for ( i = 0; i < hotKeyList.GetItemCount(); i++ )
					{
						HotKeyListItem* pTmpItem = (HotKeyListItem*)hotKeyList.GetItem( i );
						if ( pTmpItem->getHotKey() == curHotKey  && pTmpItem != pItemToSet )
						{

							// first we've got to see if we can set to the default
							long*	defaultKeys = MissionInterfaceManager::getOldKeys();
							int defaultKey = defaultKeys[pTmpItem->getCommand()];
							for ( int j = 0; j < hotKeyList.GetItemCount(); j++ )
							{
								HotKeyListItem* pCheckItem = (HotKeyListItem*)hotKeyList.GetItem( j );
								if ( pCheckItem->getHotKey() == defaultKey )
								{
									defaultKey = -1;
									break;
								}
							}			 			

							if ( defaultKey != -1 )
								oldKey = defaultKey;
							else if ( pItemToSet )
								oldKey = pItemToSet->getHotKey();

							char tmpKeyStr[256];
							tmpKeyStr[0] = 0;
							makeKeyString( oldKey, tmpKeyStr );
							pTmpItem->setHotKey( oldKey );
							pTmpItem->setKey( tmpKeyStr );
						}
					}
					pItemToSet->setKey( keysString );
					pItemToSet->setHotKey( curHotKey );
					hotKeyList.SelectItem( -1 );
				}



			}
		}

		return;
	}
	LogisticsScreen::update();
	
	hotKeyList.update();

	long tmpKey = 1;

	while( tmpKey ) // empty out keyboard buffers...
	{
		int index = hotKeyList.GetSelectedItem( );
		tmpKey = 0;

		if ( index > -1 )
		{
			HotKeyListItem* pItem = (HotKeyListItem*)hotKeyList.GetItem( index );
			tmpKey = gos_GetKey();

			if ( tmpKey )
			{
				char hotKeyString[256];
				hotKeyString[0] = 0;

				if ( 0 != makeInputKeyString( tmpKey, hotKeyString ) )
					return;

				curHotKey = tmpKey;
				// check and see if anyone else is using this one...
				for ( i = 0; i < hotKeyList.GetItemCount(); i++ )
				{
					HotKeyListItem* pTmpItem = (HotKeyListItem*)hotKeyList.GetItem( i );
					if ( pTmpItem->getHotKey() == curHotKey  && pTmpItem != pItem )
					{
						LogisticsOKDialog::instance()->setText( IDS_OPTIONS_HOTKEY_ERROR, IDS_DIALOG_NO, IDS_DIALOG_YES  );
						LogisticsOKDialog::instance()->begin();
						bShowDlg = true;
					}

				}
				
				if ( !bShowDlg )
				{
					pItem->setHotKey( tmpKey );
					pItem->setKey( hotKeyString );
					hotKeyList.SelectItem( -1 );
				}
			}

		}
	}
}

void OptionsHotKeys::makeKeyString( long newKey, char* keysString )
{
	char shift[32];
	char control[32];
	char alt[32];

	cLoadString( IDS_SHIFT, shift, 31 );
	cLoadString( IDS_CONTROL, control, 31 );
	cLoadString( IDS_ALT, alt, 31 );

	long key = newKey;
	char* pKey = gos_DescribeKey( (key & 0x000fffff) << 8 );

	if ( ((key & SHIFT)) )
	{
		strcat( keysString, shift );
		strcat( keysString, " + " );
	}

	if ( ((key & CTRL)) )
	{
		strcat( keysString, control );
		strcat( keysString, " + " );
	}
		
	if ( ((key & ALT)) )
	{
		strcat( keysString, alt );
		strcat( keysString, " + " );
	}	
	
	strcat( keysString, pKey );

}


int OptionsHotKeys::makeInputKeyString( long& tmpKey, char* hotKeyString )
{
		const char* pText = gos_DescribeKey( tmpKey & 0x0001ff00 );

		long tmp = ( tmpKey >> 8 ) & 0x01ff;
		if ( tmp == KEY_LSHIFT || tmp == KEY_LMENU || tmp == KEY_LCONTROL )
			return -1;

		long hotKey = tmp;

		bool shiftDn = userInput->shift();
		if ( shiftDn )
		{
			char shift[32];
			cLoadString( IDS_SHIFT, shift, 31 );

			hotKey |= SHIFT;
			strcat( hotKeyString, shift );
			strcat( hotKeyString, " + " );
		}
		bool ctrlDn = userInput->ctrl();
		if ( ctrlDn )
		{
			char control[32];
			cLoadString( IDS_CONTROL, control, 31 );

			hotKey |= CTRL;
			strcat( hotKeyString, control );
			strcat( hotKeyString, " + " );
		}
		bool altDn = userInput->alt();

		if ( altDn )
		{
			char alt[32];
			cLoadString( IDS_ALT, alt, 31 );

			hotKey |= ALT;
			strcat( hotKeyString, alt );
			strcat( hotKeyString, " + " );
		}

		strcat( hotKeyString, pText );
		tmpKey = hotKey;

		return 0;
}

void OptionsHotKeys::begin()
{
	helpTextArrayID = 2;
	reset(0);

}
void OptionsHotKeys::end()
{
	int i;
	for ( i= 0; i < hotKeyList.GetItemCount(); i++ )
	{
		HotKeyListItem* pItem = (HotKeyListItem*)hotKeyList.GetItem( i );
		if ( pItem )
		{
			int Command = pItem->getCommand();
			int Key = pItem->getHotKey();

			MissionInterfaceManager::setHotKey( Command, (gosEnum_KeyIndex)(Key & 0x000ffff), 
				Key & SHIFT,Key & CTRL, Key & ALT );
		}
	}

	bShowDlg= 0;

	hotKeyList.removeAllItems( true );
//	MissionInterfaceManager::setHotKey( 
}
void OptionsHotKeys::reset( bool useOld )
{
	int i;

	hotKeyList.removeAllItems( true );
	char shift[32];
	char control[32];
	char alt[32];
	char descText[128];
	char keysString[128];

	cLoadString( IDS_SHIFT, shift, 31 );
	cLoadString( IDS_CONTROL, control, 31 );
	cLoadString( IDS_ALT, alt, 31 );

	MissionInterfaceManager::Command* commands = MissionInterfaceManager::getCommands();
	long*	oldKeys = MissionInterfaceManager::getOldKeys();
	for ( i = 0; i < MAX_COMMAND; i++ )
	{
		if ( commands[i].hotKeyDescriptionText != -1 )
		{
			keysString[0] = 0;
			cLoadString( commands[i].hotKeyDescriptionText, descText, 127 );
			long key = useOld ? oldKeys[i] : commands[i].key;
			makeKeyString( key, keysString );

			HotKeyListItem* item = new HotKeyListItem();
			item->setDescription( descText );
			item->setKey( keysString );		
			item->setHotKey( key );
			item->setCommand( i );
			hotKeyList.AddItem( item );
			


		}
	}
}

//*************************************************************************************************

ScrollX::ScrollX()
{
	scrollMax = 0;
	scrollPos = 0;
	lastX = 0;
	scrollInc = 1;
	pageInc = 5;
}

long ScrollX::init(aButton* pLeft, aButton* pRight, aButton* pTab)
{
	int i;

	aObject::init( pLeft->globalX(), pLeft->top(), 
		pRight->globalRight() - pLeft->globalX(), 
		pRight->bottom() - pRight->top() );

	buttons[0] = pLeft;
	buttons[1] = pRight;
	buttons[2] = pTab;

	pLeft->setPressFX( -1 );
	pLeft->setHighlightFX( -1 );
	pLeft->setDisabledFX( -1 );

	for ( i = 0; i < 3; i++ )
		buttons[i]->setHoldTime( .01f );

	pLeft->moveTo( 0, 0 );
	pRight->moveTo( width() - pRight->width(), 0 );
	pTab->moveTo( pLeft->width() + 1, -1 );

	addChild( pLeft );
	addChild( pRight );
	addChild( pTab );

	setColor( 0 );


	return (NO_ERR);
}



void ScrollX::SetScrollMax(float newMax)
{
	scrollMax = newMax;
	buttons[2]->showGUIWindow(newMax != 0);
	ResizeAreas();
}	

void ScrollX::SetScrollPos(float newPos)
{
	if (newPos < 0)
		newPos = 0;
	if (newPos > scrollMax)
		newPos = scrollMax;
	scrollPos = newPos;
	ResizeAreas();	
	
}

void ScrollX::SetScroll( long newScrollPos )
{
	if ( newScrollPos < 0 )
		newScrollPos = 0;

	if ( newScrollPos > scrollMax )
		newScrollPos = scrollMax;

	if ( getParent() )
		getParent()->handleMessage( aMSG_SCROLLTO, newScrollPos );
	
	SetScrollPos( newScrollPos );

}
void ScrollX::update()
{
	long mouseX = userInput->getMouseX();
	long mouseY = userInput->getMouseY();

		if ( userInput->isLeftDrag() && lastX ) // dragging the little tab
		{
			float delta = (float)mouseX - (buttons[0]->globalX() + buttons[0]->width() );
			// figure out what this translates to
			float physicalRange = width() - buttons[0]->width() - buttons[1]->width() - buttons[2]->width();
			float RealRange = scrollMax;
			if ( !physicalRange )
				physicalRange = RealRange;
			float newScrollPos = .5 + (delta)*RealRange/physicalRange;
			if ( newScrollPos < 0 )
				newScrollPos = 0;
			if ( newScrollPos > scrollMax )
				newScrollPos = scrollMax;
			getParent()->handleMessage( aMSG_SCROLLTO, newScrollPos );


			SetScrollPos( newScrollPos );

		}
		else if ( pointInside( mouseX, mouseY ) )
		{
	
			if ( userInput->isLeftClick() || gos_GetKeyStatus(KEY_LMOUSE) == KEY_HELD )
			{
				lastX = 0;
				if ( buttons[2]->pointInside( mouseX, mouseY ) && 
					buttons[2]->pointInside( userInput->getMouseDragX(), userInput->getMouseDragY() ) )
					lastX = mouseX;
				else if ( getParent() )
				{
					buttons[2]->press( 0 );
					if ( !buttons[0]->pointInside( mouseX, mouseY )
						&& !buttons[1]->pointInside( mouseX, mouseY )
						&& pointInside(userInput->getMouseDragX(), userInput->getMouseDragY()) )
					{
						float physicalRange = width() - buttons[0]->width() - buttons[1]->width() - buttons[2]->width();
						float RealRange = scrollMax;
						float delta = (float)mouseX - (buttons[0]->globalX() + buttons[0]->width());
				
						// if above the thumb, page up, otherwise page down
						if ( mouseY < buttons[2]->top() )
						{
							float newScrollPos = (delta)*RealRange/physicalRange;
							if ( scrollPos - newScrollPos > pageInc )
							{
								newScrollPos = scrollPos - pageInc;
							}
							getParent()->handleMessage( aMSG_SCROLLTO, newScrollPos );
							SetScrollPos( newScrollPos );
						}
						else
						{
							float newScrollPos = (delta)*RealRange/physicalRange;
							if (  newScrollPos - scrollPos > pageInc )
							{
								newScrollPos = scrollPos + pageInc;
							}
							getParent()->handleMessage( aMSG_SCROLLTO, newScrollPos );
							SetScrollPos( newScrollPos );
						}
					}
					
				}
			}
			else
				buttons[2]->press( 0 );

		}
		else 
			buttons[2]->press( 0 );

		if ( userInput->leftMouseReleased() )
			lastX = 0;
		
	
	aObject::update();
}

int ScrollX::handleMessage( unsigned long message, unsigned long who )
{
	switch (who )
	{
		case aMSG_SCROLLUP:
			SetScrollPos( scrollPos - scrollInc );
			break;

		case aMSG_SCROLLDOWN:
			SetScrollPos( scrollPos + scrollInc );
			break;
	}

	return getParent()->handleMessage( who, who );
}

void ScrollX::ResizeAreas(void)
{
	float range, position;

	if (scrollMax == 0)
		return;

	range = width() - buttons[0]->width() - buttons[1]->width() - buttons[2]->width() - 4.f;	// one scrollwidth for buttons, one for tab. 2 for lines at either end.
	position = range * scrollPos / scrollMax;	//	center of scroll tab;

	buttons[2]->moveTo( globalX() + position + buttons[0]->width() + 2, globalY() + 1);

}

void ScrollX::Enable( bool enable )
{
	buttons[2]->disable( !enable );
	buttons[0]->disable( !enable );
	buttons[1]->disable( !enable );
}


void HotKeyListItem::init()
{
	if ( s_item )
		return;

	s_item = new HotKeyListItem();
	FitIniFile file;
	FullPathFileName path;
	path.init( artPath, "mcl_options_combobox1", ".fit" );
	if ( NO_ERR != file.open( path ) )
	{
		char error[256];
		sprintf( error, "couldn't open file %s", (char*)path );
		Assert( 0, 0, error );
		return;	
	}

	s_item->description.init( &file, "Text0" );
	s_item->text.init( &file, "Text1" );

	s_item->animations[0].init( &file, "Animation0" );
	s_item->animations[1].init( &file, "Animation1" );
	s_item->animations[2].init( &file, "Animation2" );

	s_item->rects[0].init( &file, "Rect0" );
	s_item->rects[1].init( &file, "Rect1" );
}
void HotKeyListItem::render()
{
	int i;


	aAnimGroup::STATE curState = (aAnimGroup::STATE)getState();

	for ( i = 0; i < 3; i++ )
	{
		animations[i].setState( curState );
		animations[i].update();
	}
	text.setColor( animations[1].getCurrentColor( curState ) );
	description.setColor( animations[0].getCurrentColor( curState ) );
	rects[1].setColor( animations[2].getCurrentColor( curState ) );
	aObject::render();
}

void HotKeyListItem::update()
{

}

void HotKeyListItem::setDescription( const char* pText )
{
	description.setText( pText );
}
void HotKeyListItem::setKey( const char* pText )
{
	text.setText( pText );
}
HotKeyListItem::~HotKeyListItem() 
{
	removeAllChildren( 0 );
}
HotKeyListItem::HotKeyListItem( )
{
	int i;
	if ( s_item )
	{
		description = s_item->description;
		text = s_item->text;
		for ( i = 0; i < 3; i++ )
			animations[i] = s_item->animations[i];

		rects[0] = s_item->rects[0];
		rects[1] = s_item->rects[1];
		aObject::init( 0, 0, rects[1].right(), rects[0].bottom()+1 );

		addChild( &rects[1] );
		addChild( &description );
		addChild( &text );
		addChild( &rects[0] );


	}
}


