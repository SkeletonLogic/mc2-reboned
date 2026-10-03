//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include <io.h> // for _chmod()

#include "prefs.h"
#include "gamesound.h"
#include "missiongui.h" // PORT: match case-sensitive host path.

#include "Soundsys.h" // PORT: match case-sensitive host path.
#include "../resource.h" // PORT: use host-independent include separators.

extern SoundSystem *sndSystem;

extern long DigitalMasterVolume;
extern long MusicVolume;
extern long sfxVolume;
extern long RadioVolume;
extern long BettyVolume;
extern bool useShadows;
extern bool useLocalShadows;
extern bool useWaterInterestTexture;

extern bool useUnlimitedAmmo;
extern long GameDifficulty;
extern long renderer;
extern long resolution;
extern long gammaLevel;
extern bool useLeftRightMouseProfile;
extern bool useNonWeaponEffects;
extern bool useHighObjectDetail;
extern bool useFog;

extern long GameVisibleVertices;

extern volatile bool mc2UseAsyncMouse;		//Should mouse draw and update in separate thread?


extern DWORD gEnableDetailTexture;

CPrefs::CPrefs() {
	int i;
	DigitalMasterVolume = 255;
	MusicVolume = 64;
	sfxVolume = 64;
	RadioVolume = 64;
	BettyVolume = 64;

	useShadows = true;
	useWaterInterestTexture = true;
	useHighObjectDetail = true;

	GameDifficulty = 0;
	useUnlimitedAmmo = true;

	renderer = 0;
	resolution = 1;
	// PORT: the original default remains 800x600 when older options.cfg files omit dimensions.
	screenWidth = 800;
	screenHeight = 600;
	fullScreen = false;
	gammaLevel = 0;
	useLeftRightMouseProfile = true; // if false, use old style commands
	fogPos = 50;
	useFog = true;
	renderDistancePos = 50;
	for ( i = 0; i < 10; i++ )
		playerName[i][0] = 0;

	for (i = 0; i < 10; i++ )
		ipAddresses[i][0] = 0;


	baseColor = 0xffff7e00;
	highlightColor = 0xffff7e00;
	faction = 0;
	insigniaFile[0] = 0;
	for ( i = 0; i < 10; i++ )
		unitName[i][0] = 0;

#if 0
	FilterState = gos_FilterNone;
	TERRAIN_TXM_SIZE = 64;
	ObjectTextureSize = 128;
	useRealLOS = true;
	doubleClickThreshold = 0.2f;
	dragThreshold = 10;
	BaseVertexColor = 0x00000000;		//This color is applied to all vertices in game as Brightness correction.
#endif
}

int CPrefs::load( const char* pFileName ) {
	int i;
	long result = 0;

	FullPathFileName prefsPathname;
	prefsPathname.init("./",pFileName,".cfg");
	FitIniFilePtr prefsFile = new FitIniFile;

#ifdef _DEBUG
	long prefsOpenResult = 
#endif
	prefsFile->open(prefsPathname);

	gosASSERT (prefsOpenResult == NO_ERR);
	{
#ifdef _DEBUG
		long prefsBlockResult = 
#endif
			prefsFile->seekBlock("MechCommander2");
		gosASSERT(prefsBlockResult == NO_ERR);
		{
			// store volume settings in global variable since soundsystem 
			// does not exist yet.  These will be set in SoundSystem::init()
			result = prefsFile->readIdLong("DigitalMasterVolume",DigitalMasterVolume);
			if (result != NO_ERR)
				DigitalMasterVolume = 255;

			result = prefsFile->readIdLong("MusicVolume",MusicVolume);
			if (result != NO_ERR)
				MusicVolume = 64;

			result = prefsFile->readIdLong("RadioVolume",RadioVolume);
			if (result != NO_ERR)
				RadioVolume = 64;

			result = prefsFile->readIdLong("SFXVolume",sfxVolume);
			if (result != NO_ERR)
				sfxVolume = 64;

			result = prefsFile->readIdLong("BettyVolume",BettyVolume);
			if (result != NO_ERR)
				BettyVolume = 64;

			result = prefsFile->readIdBoolean( "Shadows", useShadows);
			if (result != NO_ERR)
				useShadows = true;

			result = prefsFile->readIdBoolean( "DetailTexture", useWaterInterestTexture);
			if (result != NO_ERR)
				useWaterInterestTexture = true;

			result = prefsFile->readIdBoolean( "HighObjectDetail", useHighObjectDetail );
			if ( result != NO_ERR )
				useHighObjectDetail = true;

			result = prefsFile->readIdLong("Difficulty",GameDifficulty);
			if (result != NO_ERR)
				GameDifficulty = 1;

			result = prefsFile->readIdBoolean("UnlimitedAmmo",useUnlimitedAmmo);
			if (result != NO_ERR)
				useUnlimitedAmmo = true;

			result = prefsFile->readIdLong("Rasterizer",renderer);
			if (result != NO_ERR)
				renderer = 0;

			if ((renderer < 0) || (renderer > 3))
				renderer = 0;

			//Force use of video card which is above min spec.
			// Used ONLY if they are using a below minSpec Primary with an above minSpec secondary.
			if ((renderer >= 0) && (renderer < 3) && 
				(gos_GetMachineInformation(gos_Info_GetDeviceLocalMemory, renderer) +
				gos_GetMachineInformation(gos_Info_GetDeviceAGPMemory, renderer)) < 6291456)
			{
				long testRender = 0;
				while (testRender < 3)
				{
					if ((gos_GetMachineInformation(gos_Info_GetDeviceLocalMemory, testRender) +
						gos_GetMachineInformation(gos_Info_GetDeviceAGPMemory, testRender)) >= 6291456)
						break;

					testRender++;
				}

				if (testRender == 3)
				{
					//No video card in the machine above MinSpec.  Just set it to the first one.
					renderer = 0;
				}
			}
				
			result = prefsFile->readIdLong("Resolution",resolution);
			if (result != NO_ERR)
				resolution = 1;

			// PORT: migrate the retail index to exact dimensions, then allow new configs
			// to override both values.  Requiring a valid pair prevents half-written
			// options files from producing an unusable mode.
			switch (resolution)
			{
				case 0: screenWidth = 640;  screenHeight = 480;  break;
				case 2: screenWidth = 1024; screenHeight = 768;  break;
				case 3: screenWidth = 1280; screenHeight = 1024; break;
				case 4: screenWidth = 1600; screenHeight = 1200; break;
				// Resolution 5 means "a mode from the port's modern catalogue",
				// not 1920x1080 specifically -- every added entry writes 5. The
				// exact pair below is what actually carries the choice, so a 5
				// with no readable pair is an incomplete config, and the honest
				// answer is the retail default rather than a guessed 1920x1080
				// the display may not even have.
				case 5: screenWidth = 800; screenHeight = 600; break;
				default:
					resolution = 1;
					screenWidth = 800;
					screenHeight = 600;
					break;
			}
			long savedWidth = screenWidth;
			long savedHeight = screenHeight;
			const long widthResult = prefsFile->readIdLong("ScreenWidth", savedWidth);
			const long heightResult = prefsFile->readIdLong("ScreenHeight", savedHeight);
			if (widthResult == NO_ERR && heightResult == NO_ERR &&
				savedWidth >= 640 && savedHeight >= 480 &&
				savedWidth <= 16384 && savedHeight <= 16384)
			{
				screenWidth = savedWidth;
				screenHeight = savedHeight;
			}

			result = prefsFile->readIdBoolean("FullScreen",fullScreen);
			if (result != NO_ERR)
				fullScreen = false;

			result = prefsFile->readIdLong("Brightness",gammaLevel);
			if (result != NO_ERR)
				gammaLevel = 0;

			result = prefsFile->readIdBoolean( "useLeftRightMouseProfile", useLeftRightMouseProfile );
			if ( result != NO_ERR )
				useLeftRightMouseProfile = true;

			char blockName[64];
			for ( i = 0; i < 10; i++ )
			{	
				sprintf( blockName, "PlayerName%ld", i );
				result = prefsFile->readIdString( blockName, &playerName[i][0], 255 );

				if ( result != NO_ERR && i == 0 )
				{
					result = prefsFile->readIdString( "PlayerName", &playerName[0][0], 255 );
					result = prefsFile->readIdString( "UnitName", &unitName[0][0], 255 );
					break;
				}

				sprintf( blockName, "IPAddress%ld", i );
				result = prefsFile->readIdString( blockName, &ipAddresses[i][0], 255 );


				sprintf( blockName, "UnitName%ld", i );
				result = prefsFile->readIdString( blockName, &unitName[i][0], 255 );

			}

		
			result = prefsFile->readIdLong( "BaseColor", baseColor );
			if ( result != NO_ERR )	
				baseColor = 0xffff7e00;
			result = prefsFile->readIdLong( "Highlightcolor", highlightColor );
			if ( result != NO_ERR )	
				highlightColor = 0xffff7e00;
			
			result = prefsFile->readIdLong( "faction", faction );
			result = prefsFile->readIdString( "InsigniaFile", insigniaFile, 255 );

			result = prefsFile->readIdBoolean( "PilotVideos",	pilotVideos );
			if ( result != NO_ERR )
			{
				pilotVideos = true;
			}
			result = prefsFile->readIdBoolean( "UseLocalShadows",	useLocalShadows );
			if ( result != NO_ERR )
			{
				useLocalShadows = true;
			}
			result = prefsFile->readIdBoolean( "UseNonWeaponEffects",	useNonWeaponEffects );
			if ( result != NO_ERR )
			{
				useNonWeaponEffects = 0;
			}
			result = prefsFile->readIdBoolean( "AsyncMouse",	asyncMouse );
			if ( result != NO_ERR )
				asyncMouse = 0;

			result = prefsFile->readIdLong( "FogPos",	fogPos );
			if ( result != NO_ERR )
			{
				fogPos = 50;
			}
			fogPos = clampPercent(fogPos);
			result = prefsFile->readIdBoolean( "UseFog", useFog );
			if ( result != NO_ERR )
				useFog = true; // Older options.cfg files preserve retail appearance.
			// PORT: migrate without changing an existing player's view distance.
			// The extended control's midpoint is the legacy maximum, so halve the
			// old FogPos percentage when the independent key is not present.
			renderDistancePos = (fogPos + 1) / 2;
			result = prefsFile->readIdLong( "RenderDistancePos", renderDistancePos );
			renderDistancePos = clampPercent(renderDistancePos);
			result = prefsFile->readIdChar( "BitDepth",	bitDepth );
			if ( result != NO_ERR )
				bitDepth = 0;

			if (bitDepth && (renderer == 3))
				bitDepth = 0;

			result = prefsFile->readIdBoolean( "SaveTranscripts", saveTranscripts );
			result = prefsFile->readIdBoolean( "Tutorials", tutorials );
			if ( result != NO_ERR )
				tutorials = true;

		}

	}
#ifndef VIEWER
	MissionInterfaceManager::loadHotKeys( *prefsFile);
#endif
	prefsFile->close();
	
	delete prefsFile;
	prefsFile = NULL;

	return 0;
}

int CPrefs::save() {
	int i;
	char backupPath[256];
	char originalPath[256];
	
	strcpy( originalPath, "options.cfg" );
	strcpy( backupPath, originalPath );
	strcat( backupPath,".old" );
	_chmod(originalPath, _S_IWRITE);
	_chmod(backupPath, _S_IWRITE);
	remove(backupPath);
	rename(originalPath, backupPath);
	//MoveFileEx(originalPath, backupPath, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);

	FitIniFilePtr prefsFile = new FitIniFile;
	int result = prefsFile->create( (char*)originalPath );
	if (result != NO_ERR)
	{
		gosASSERT( false );
		return -1;
	}

	{
#ifdef _DEBUG
		long prefsBlockResult = 
#endif
		prefsFile->writeBlock("MechCommander2");
		gosASSERT(prefsBlockResult == strlen("\r\n[") + strlen("MechCommander2") + strlen("]\r\n"));
		{
			result = prefsFile->writeIdLong("DigitalMasterVolume",DigitalMasterVolume);
			result = prefsFile->writeIdLong("MusicVolume",MusicVolume);
			result = prefsFile->writeIdLong("RadioVolume",RadioVolume);
			result = prefsFile->writeIdLong("SFXVolume",sfxVolume);
			result = prefsFile->writeIdLong("BettyVolume",BettyVolume);

			result = prefsFile->writeIdBoolean( "Shadows", useShadows );
			result = prefsFile->writeIdBoolean( "DetailTexture", useWaterInterestTexture );
			result = prefsFile->writeIdBoolean( "HighObjectDetail", useHighObjectDetail );

			result = prefsFile->writeIdLong("Difficulty",GameDifficulty);
			result = prefsFile->writeIdBoolean("UnlimitedAmmo",useUnlimitedAmmo);

			result = prefsFile->writeIdLong("Rasterizer",renderer);
			result = prefsFile->writeIdLong("Resolution",resolution);
			// PORT: exact dimensions make widescreen a persistent player choice;
			// Resolution remains above for backward compatibility with old tools.
			result = prefsFile->writeIdLong("ScreenWidth",screenWidth);
			result = prefsFile->writeIdLong("ScreenHeight",screenHeight);
			result = prefsFile->writeIdBoolean("FullScreen",fullScreen);
			result = prefsFile->writeIdLong("Brightness",gammaLevel);
			result = prefsFile->writeIdBoolean( "useLeftRightMouseProfile", useLeftRightMouseProfile );
			char blockName[64];
			for ( i = 0; i < 10; i++ )
			{	
				
				sprintf( blockName, "PlayerName%ld", i );
				result = prefsFile->writeIdString( blockName, &playerName[i][0] );

				sprintf( blockName, "UnitName%ld", i );
				result = prefsFile->writeIdString( blockName, &unitName[i][0] );

				sprintf( blockName, "IPAddress%ld", i );
				result = prefsFile->writeIdString( blockName, &ipAddresses[i][0] );

			}

			result = prefsFile->writeIdLong( "BaseColor", baseColor );
			result = prefsFile->writeIdLong( "Highlightcolor", highlightColor );
			result = prefsFile->writeIdLong( "faction", faction );
			result = prefsFile->writeIdString( "InsigniaFile", insigniaFile );

			result = prefsFile->writeIdBoolean( "PilotVideos",	pilotVideos );
			result = prefsFile->writeIdBoolean( "UseLocalShadows",	useLocalShadows );
			result = prefsFile->writeIdBoolean( "UseNonWeaponEffects",	useNonWeaponEffects );
			result = prefsFile->writeIdBoolean( "AsyncMouse",	asyncMouse );
			result = prefsFile->writeIdLong( "FogPos",	fogPos );
			result = prefsFile->writeIdBoolean( "UseFog", useFog );
			result = prefsFile->writeIdLong( "RenderDistancePos", renderDistancePos );
			result = prefsFile->writeIdChar( "BitDepth",	bitDepth );

			result = prefsFile->writeIdBoolean( "SaveTranscripts", saveTranscripts );
			result = prefsFile->writeIdBoolean( "Tutorials", tutorials );

		}
	}
#ifndef VIEWER	
	MissionInterfaceManager::saveHotKeys( *prefsFile);
#endif
	prefsFile->close();
	
	
	delete prefsFile;
	prefsFile = NULL;

	return 0;
}

long CPrefs::clampPercent(long value)
{
	if (value < 0) return 0;
	if (value > 100) return 100;
	return value;
}

float CPrefs::renderClipDistance(long value)
{
	// PORT: retain the legacy 3000-5000 range in the lower half. The upper half
	// clears the elevated owner capture's projected 13400-unit horizon plus the
	// measured 1650-unit maximum campaign elevation span, with added margin.
	const long percent = clampPercent(value);
	if (percent <= 50)
		return 3000.0f + 40.0f * float(percent);
	return 5000.0f + 260.0f * float(percent - 50);
}

long CPrefs::renderVisibleVertices(long value)
{
	// PORT: 160 vertices cover the corresponding low-angle terrain footprint
	// while avoiding the timing loss of rendering the entire map.
	const long percent = clampPercent(value);
	if (percent <= 50)
		return 30 + (3 * percent) / 5;
	return 60 + 2 * (percent - 50);
}

int CPrefs::applyPrefs(bool applyRes) {
	if (soundSystem) {
		/*soundSystem doesn't seem to do anything*/
		soundSystem->setDigitalMasterVolume(this->DigitalMasterVolume);
		soundSystem->setSFXVolume(this->sfxVolume);
		soundSystem->setRadioVolume(this->RadioVolume);
		soundSystem->setMusicVolume(this->MusicVolume);
		soundSystem->setBettyVolume(this->BettyVolume);
	}

	if (sndSystem) {
		sndSystem->setDigitalMasterVolume(this->DigitalMasterVolume);
		sndSystem->setSFXVolume(this->sfxVolume);
		sndSystem->setRadioVolume(this->RadioVolume);
		sndSystem->setMusicVolume(this->MusicVolume);
		sndSystem->setBettyVolume(this->BettyVolume);
	}

	::useShadows = this->useShadows;
	::useLocalShadows = this->useLocalShadows;
	::useWaterInterestTexture = this->useWaterInterestTexture;

	::GameDifficulty = this->GameDifficulty;
	::useUnlimitedAmmo = this->useUnlimitedAmmo;

	::renderer = this->renderer;
	::resolution = this->resolution;
	::gammaLevel = this->gammaLevel;
	mc2UseAsyncMouse = this->asyncMouse;
	::useLeftRightMouseProfile = this->useLeftRightMouseProfile;
	::useNonWeaponEffects = this->useNonWeaponEffects;
	::useHighObjectDetail = this->useHighObjectDetail;
	::useFog = this->useFog;

	int bitDepth = this->bitDepth ? 32 : 16;

	// PORT: haze is cosmetic; view distance and terrain work have one separate owner.
	float fogPercent = float(clampPercent(fogPos)) / 100.0f;
	Camera::MinHazeDistance	= 2000.0f + (2000.0f * fogPercent);
	// Keep the fog ramp's far edge on the current render horizon. Assigning
	// MaxClipDistance directly leaves Camera::DistanceFactor at its startup
	// 3000-4000 range, which makes the haze fully opaque near the camera even
	// when the port's extended view distance reaches 18000.
	Camera::MaxClipDistance = renderClipDistance(renderDistancePos);
	if (::useFog)
		Camera::DistanceFactor = 1.0f / (Camera::MaxClipDistance - Camera::MinHazeDistance);

	// PORT (M8-B15): report the haze ramp against the horizon it is meant to
	// reach. HazeFactor is (distance - MinHazeDistance) * DistanceFactor, so it
	// saturates at MinHazeDistance + 1/DistanceFactor. If that saturation point
	// falls far SHORT of MaxClipDistance, everything past it draws fully hazed
	// while the renderer still believes it is well inside the view distance --
	// exactly what the comment above warns about, and the correction runs only
	// when ::useFog is true.
	//
	// CAN COME BACK NEGATIVE: if saturation lands on MaxClipDistance the ramp is
	// correct and the missing objects come from somewhere else.
	{
		const char* wantHaze = getenv("MC2_CAMERA_CENSUS");
		if (wantHaze && *wantHaze && strcmp(wantHaze, "0"))
		{
			const float saturateAt = Camera::DistanceFactor > 0.0f
				? Camera::MinHazeDistance + (1.0f / Camera::DistanceFactor)
				: -1.0f;
			printf("[camera] haze ramp: useFog=%s fogPos=%d renderDistancePos=%d\n",
			       ::useFog ? "true" : "false", (int)fogPos, (int)renderDistancePos);
			printf("[camera]   MinHazeDistance=%.1f MaxClipDistance=%.1f DistanceFactor=%.8f\n",
			       Camera::MinHazeDistance, Camera::MaxClipDistance, Camera::DistanceFactor);
			printf("[camera]   haze saturates at %.1f vs horizon %.1f  ->  %s\n",
			       saturateAt, Camera::MaxClipDistance,
			       (saturateAt > 0.0f && saturateAt < Camera::MaxClipDistance * 0.95f)
			           ? "SATURATES SHORT OF THE HORIZON" : "ramp reaches the horizon");
			fflush(stdout);
		}
	}
	::GameVisibleVertices = renderVisibleVertices(renderDistancePos);

	if (land)
	{
		land->resetVisibleVertices(::GameVisibleVertices);
		land->update();
	}

	if ( applyRes )
	{
		long localRenderer = renderer;
		if ((renderer != 0) && (renderer != 3))
			localRenderer = 0;

		bool localFullScreen = fullScreen;
		bool localWindow = !fullScreen;
		if (Environment.fullScreen && fullScreen)
			localFullScreen = false;

		// PORT: apply the exact saved dimensions instead of collapsing every mode
		// through the retail five-entry resolution switch.
		//
		// The retail 1600x1200 case passed a hard-coded 16 where every other case
		// passed the variable; that inconsistency is not preserved here, so
		// 1600x1200 can now be 32-bit like the other four. Deliberate, and the
		// only behavioural difference against pristine in this function.
		long applyWidth = screenWidth;
		long applyHeight = screenHeight;

		if ((gos_GetMachineInformation( gos_Info_ValidMode, renderer,
			applyWidth, applyHeight, bitDepth) == 0) && (bitDepth == 32))
			bitDepth = 16;

		// PORT: a saved mode this machine cannot display must not be applied.
		// options.cfg travels between machines and the resolution catalogue
		// reaches 5120x2880, so without this a config written on a 4K desktop
		// hands an impossible size to gos_SetScreenMode on a 1080p one -- and the
		// retail switch it replaced could at least fall through and do nothing.
		// 800x600 is the retail default and the size every menu is authored at.
		if (gos_GetMachineInformation( gos_Info_ValidMode, renderer,
			applyWidth, applyHeight, bitDepth) == 0)
		{
			applyWidth = 800;
			applyHeight = 600;
		}

		if (renderer == 3)
			gos_SetScreenMode(applyWidth,applyHeight,bitDepth,0,0,0,true,localFullScreen,0,localWindow,0,localRenderer);
		else
			gos_SetScreenMode(applyWidth,applyHeight,bitDepth,renderer,0,0,0,localFullScreen,0,localWindow,0,localRenderer);
	}

	return 0;
}

void CPrefs::setNewName( const char* pNewName )
{
	int i;
	if ( !pNewName )
		return;
	// check and see if this name is already in here
	for ( i = 0; i < 10; i++ )
	{
		if ( !stricmp( pNewName, playerName[i] ) )
		{
			// found the same one so now we just shuffle
			for ( int j = i; j < 9; j++ )
			{
				strcpy( playerName[j], playerName[j+1] );
			}

			memmove( playerName[1], playerName[0], sizeof( char ) * 9 * 256 );
			strcpy( playerName[0], pNewName );

			break;
		}
		else if ( !strlen( playerName[i] ) )
		{
			// found the last one...

			memmove( playerName[1], playerName[0], sizeof( char ) * 9 * 256 );
			strcpy( playerName[0], pNewName );

			break;
		}
	}

	if ( i == 10 )
	{
		memmove( playerName[1], playerName[0], sizeof( char ) * 9 * 256 );
		strcpy( playerName[0], pNewName );
	}
}

void CPrefs::setNewIP( const char* pNewIP )
{
	int i;
	if ( !pNewIP )
		return;
	// check and see if this name is already in here
	for ( i = 0; i < 10; i++ )
	{
		if ( !stricmp( pNewIP, ipAddresses[i] ) )
		{
			// found the same one so now we just shuffle
			for ( int j = i; j < 9; j++ )
			{
				strcpy( ipAddresses[j], ipAddresses[j+1] );
			}

			memmove( ipAddresses[1], ipAddresses[0], sizeof( char ) * 9 * 24 );
			strcpy( ipAddresses[0], pNewIP );

			break;
		}
		else if ( !strlen( ipAddresses[i] ) )
		{
			// found the last one...

			memmove( ipAddresses[1], ipAddresses[0], sizeof( char ) * 9 * 24 );
			strcpy( ipAddresses[0], pNewIP );

			break;
		}
	}
}

void CPrefs::setNewUnit( const char* pNewUnit )
{
	int i;
	if ( !pNewUnit )
		return;
// check and see if this name is already in here
	for ( i = 0; i < 10; i++ )
	{
		if ( !stricmp( pNewUnit, unitName[i] ) )
		{
			// found the same one so now we just shuffle
			for ( int j = i; j < 9; j++ )
			{
				strcpy( unitName[j], unitName[j+1] );
			}

			memmove( unitName[1], unitName[0], sizeof( char ) * 9 * 256 );
			strcpy( unitName[0], pNewUnit );

			break;
		}
		else if ( !strlen( unitName[i] ) )
		{
			// found the last one...

			memmove( unitName[1], unitName[0], sizeof( char ) * 9 * 256 );
			strcpy( unitName[0], pNewUnit );

			break;
		}
	}

	if ( i == 10 )
	{
		memmove( unitName[1], unitName[0], sizeof( char ) * 9 * 256 );
		strcpy( unitName[0], pNewUnit );
	}
}



