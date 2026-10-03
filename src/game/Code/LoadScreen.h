#ifndef LOADSCREEN_H
#define LOADSCREEN_H
/*************************************************************************************************\
LoadScreen.h			: Interface for the LoadScreen component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#ifndef LOGISTICSSCREEN_H
#include "LogisticsScreen.h"
#endif

#ifndef AANIM_H
#include "AANIM.H" // PORT: match case-sensitive host path.
#endif

struct TGAFileHeader;
class FitIniFile;

struct tagRECT;
struct _DDSURFACEDESC2;



//*************************************************************************************************
class LoadScreen;
/**************************************************************************************************
CLASS DESCRIPTION
LoadScreen:
**************************************************************************************************/
// PORT (M8-B50/B51): pick the widescreen loading layout when the display is
// wider than 4:3 and the art is present. Writes the base name (no extension)
// into the buffer and returns true. BOTH the entrance screen (MissionBegin.cpp)
// and the exit screen (LoadScreenWrapper::changeRes) must call this, or one
// half of the loading screen uses retail art and the other uses the rebuild.
bool MC2_SelectWideLoadScreen( char* fileNameInOut );

class LoadScreenWrapper : public LogisticsScreen
{
public:

	LoadScreenWrapper();

	virtual ~LoadScreenWrapper();
	
	void init( FitIniFile& file );

	virtual void update();
	virtual void render( int xOffset, int yOffset );

	virtual void begin();

	static LoadScreen*	enterScreen;
	static LoadScreen*	exitScreen;
	static void			changeRes();
	bool				waitForResChange;
	bool				bFirstTime;
	bool				useExitScreen; // PORT: phase cannot be inferred from physical widescreen width.
};


class LoadScreen: public LogisticsScreen
{
	public:

		LoadScreen();
		virtual ~LoadScreen();

		void init( FitIniFile& file, DWORD neverFlush = 0 );
		virtual void update();
		virtual void render( int xOffset, int yOffset );

		virtual void begin();

		void resetExitAnims();
		void setupOutAnims();

		static void changeRes(FitIniFile& file);
		


	private:

		LoadScreen( const LoadScreen& src );
		LoadScreen& operator=( const LoadScreen& oadScreen );


		static TGAFileHeader*	progressTextureMemory;
		static TGAFileHeader*	progressBackground;
		static TGAFileHeader*	mergedTexture;
		static TGAFileHeader*	waitingForPlayersMemory;

		static long xProgressLoc;
		static long yProgressLoc;
		static long xWaitLoc;
		static long yWaitLoc;

		friend void ProgressTimer( tagRECT& WinRect, _DDSURFACEDESC2&  );
		friend class Mission;

		//Must needs be static cause we create a new one when the res changes!!
		// This will erase the hardmouse status and we always use async mouse then!
		static bool	turnOffAsyncMouse;

		aAnimation	outAnims[5];
		aAnimation	inAnims[5];
		aText		text;
		int*		animIndices;


};


//*************************************************************************************************
#endif  // end of file ( LoadScreen.h )
