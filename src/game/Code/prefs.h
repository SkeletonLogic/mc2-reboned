//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef PREFS_H
#define PREFS_H

#ifndef MCLIB_H
#include "mclib.h"
#endif


class CPrefs {
public:
	CPrefs();
	int load( const char* pFileName = "options");
	int save();
	int applyPrefs(bool bApplyResolution = 1);
	static long clampPercent(long value);
	static float renderClipDistance(long value);
	static long renderVisibleVertices(long value);

	void setNewName( const char* pNewName );
	void setNewUnit( const char* pNewUnit );
	void setNewIP( const char* pNewIP );

public:
	long DigitalMasterVolume;
	long MusicVolume;
	long sfxVolume;
	long RadioVolume;
	long BettyVolume;

	bool useShadows;
	bool useWaterInterestTexture;
	bool useHighObjectDetail;

	long GameDifficulty;
	bool useUnlimitedAmmo;

	long renderer;
	long resolution;
	// PORT: retain the legacy resolution index while persisting exact widescreen dimensions.
	long screenWidth;
	long screenHeight;
	bool fullScreen;
	long gammaLevel;
	bool useLeftRightMouseProfile; // if false, use old style commands
	long baseColor;
	long highlightColor;
	long faction;
	char insigniaFile[256];
	char unitName[10][256];
	char playerName[10][256];
	char ipAddresses[10][24];

	bool	pilotVideos;
	bool	useNonWeaponEffects;
	bool	useLocalShadows;
	bool	asyncMouse;
	long	fogPos;
	bool	useFog; // PORT (M8-3): player-facing distance-fog toggle.
	long	renderDistancePos; // PORT: independent view distance; 0..100.
	char	bitDepth; // 0 == 16, 1 == 32

	bool	saveTranscripts;
	bool	tutorials;

#if 0
	long FilterState;
	long TERRAIN_TXM_SIZE;
	long ObjectTextureSize;
	bool	useRealLOS;
	float doubleClickThreshold;
	long dragThreshold;
	DWORD BaseVertexColor;		//This color is applied to all vertices in game as Brightness correction.
#endif
};

extern CPrefs prefs;
#endif /*PREFS_H*/
