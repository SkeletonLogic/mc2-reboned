#define LOADSCREEN_CPP
/*************************************************************************************************\
LoadScreen.cpp			: Implementation of the LoadScreen component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include <windows.h>
#include <ddraw.h>
#include "LoadScreen.h"
#include "aAnimObject.h"
#include "tgainfo.h" // PORT: match case-sensitive host path.
#include "mclib.h"
#include "prefs.h" // PORT: match case-sensitive host path.
#include "multplyr.h"
#include "mission.h"
#include "gamesound.h" // PORT: match case-sensitive host path.
#include "mc2_port.h" // PORT: loading art follows the fixed logistics canvas.
float loadProgress = 0.0f;

long LoadScreen::xProgressLoc = 0;
long LoadScreen::yProgressLoc = 0;
long LoadScreen::xWaitLoc = 0;
long LoadScreen::yWaitLoc = 0;
bool LoadScreen::turnOffAsyncMouse = false;

TGAFileHeader* LoadScreen::progressTextureMemory = 0;
TGAFileHeader* LoadScreen::progressBackground = 0;
TGAFileHeader* LoadScreen::mergedTexture = 0;

// PORT (M8-B49): THE PROGRESS BAR HAS NEVER DRAWN, AND NOT FOR THE REASON I
// CHASED FIRST.
//
// Retail blits it from ProgressTimer() straight into a LOCKED DIRECTDRAW FRONT
// BUFFER. mouse.cpp:223 only calls AsynFunc when FrontBufferSurface->Lock()
// succeeds, and this port sets FrontBufferSurface = nullptr
// (gos_platform.cpp:207). So the lock fails, AsynFunc is never invoked, and
// ProgressTimer has never run once. Nothing about the bar VALUE was ever wrong --
// the function that draws it is not called at all.
//
// So draw it the way everything else on this screen is drawn: two ordinary GUI
// quads, the empty bar and the filled bar clipped to loadProgress. Pointers
// rather than objects so there is no static-initialisation order to reason about.
static aObject* progressBarBack = NULL;
static aObject* progressBarFill = NULL;
static long     progressBarW = 0, progressBarH = 0;
static long     progressBarU = 0, progressBarV = 0;

// PORT (M8-B49): set a quad's UVs correctly on a NON-SQUARE texture.
//
// aObject::setUVs divides BOTH u and v by fileWidth, and fileWidth is the
// texture's WIDTH. That is only right for a square texture. The bar art is
// 298x32, so asking for v = 32 gave 32/298 = 0.107 of the height: the top tenth
// of the bar stretched over the whole rect, with the rest never sampled. That is
// the "partially cut off at the bottom" the owner reported.
//
// Scaling v by width/height cancels the wrong divisor exactly.
static void MC2_SetBarUVs( aObject* obj, long u, long v, long uw, long vh )
{
	if ( !obj )
		return;
	const float texW = obj->getFileWidth();
	float texH = texW;
	unsigned long gosID = mcTextureManager->get_gosTextureHandle( obj->getTextureHandle() );
	if ( gosID )
	{
		TEXTUREPTR textureData;
		gos_LockTexture( gosID, 0, 0, &textureData );
		if ( textureData.Height )
			texH = (float)textureData.Height;
		gos_UnLockTexture( gosID );
	}
	const float vScale = ( texH > 0.f ) ? ( texW / texH ) : 1.f;
	obj->setUVs( (float)u, v * vScale, (float)( u + uw ), ( v + vh ) * vScale );
}



// PORT (M8-B50): is the widescreen loading art in use, and how big is the canvas
// it was authored for? The artwork is its own coordinate space -- authoring in it
// means no resampling of the owner's art and no letterbox, because the engine
// scales the whole canvas to the window as it already does for 800x600.
bool MC2_wideLoadScreen = false;
// PORT (M8-B53): the widescreen layout is authored in the SAME 800x600 canvas
// coordinates every other screen uses -- its objects simply run outside 0..800.
// Do NOT switch the canvas for it: the screen behind the doors is authored for
// 800x600 and a canvas swap rescales that too, which the owner saw as the game
// UI resizing behind the closing doors.

// PORT (M8-B50): choose the widescreen layout, for BOTH halves of the screen.
//
// Retail has loading art for the 4:3 tiers only, and logistics runs on an 800x600
// virtual canvas, so at 16:9 the authored screen is scaled by HEIGHT and leaves
// bare columns either side -- 240px each at 1920x1080, with the mission rendering
// into them. The owner supplied a 16:9 rebuild; tools/loadscreen-wide-build.py
// cuts it into tiles and writes mcl_loadingscreen_wide.fit in the artwork's own
// coordinates.
//
// M8-B51: this MUST be consulted by the entrance screen too. MissionBegin.cpp
// opens the .fit by name and hands it to LoadScreenWrapper::init, while the exit
// screen is built later in LoadScreenWrapper::changeRes. Wire up only the second
// and the two halves of one loading screen disagree -- which is what shipped:
// the retail art slid in, then the HD art replaced it on the way out.
//
// Selected only when the display really is wider than 4:3 AND the file is
// present, so a stock install is untouched and a missing file falls back to the
// retail layout rather than failing to open.
bool MC2_SelectWideLoadScreen( char* fileNameInOut )
{
	MC2_wideLoadScreen = false;
	if ( prefs.screenHeight > 0 &&
		 ((float)prefs.screenWidth / (float)prefs.screenHeight) > 1.4f )
	{
		FullPathFileName widePath;
		widePath.init( artPath, "mcl_loadingscreen_wide", ".fit" );
		File probe;
		if ( NO_ERR == probe.open( widePath ) )
		{
			probe.close();
			if ( fileNameInOut )
				strcpy( fileNameInOut, "mcl_loadingscreen_wide" );
			MC2_wideLoadScreen = true;
		}
	}
	return MC2_wideLoadScreen;
}

TGAFileHeader* LoadScreen::waitingForPlayersMemory = 0;

LoadScreen* LoadScreenWrapper::enterScreen = NULL;
LoadScreen* LoadScreenWrapper::exitScreen = NULL;


extern volatile bool mc2IsInMouseTimer;
extern volatile bool mc2IsInDisplayBackBuffer;

extern void (*AsynFunc)(RECT& WinRect,DDSURFACEDESC2& mouseSurfaceDesc );
// PORT: forward declaration for ProgressTimer. It is only *friend*-declared in
// LoadScreen.h, which does not introduce the name for ordinary lookup, and its
// definition sits further down this file.
void ProgressTimer( RECT& WinRect, DDSURFACEDESC2& mouseSurfaceDesc );
extern CPrefs prefs;

//
// Returns the number of bits in a given mask.  Used to determine if we are in 555 mode vs 565 mode.
WORD GetNumberOfBits( DWORD dwMask );

void MouseTimerInit();
void MouseTimerKill();

LoadScreenWrapper::LoadScreenWrapper()
{
	//How about, for shits and giggles we toss the static members we created for single player when this inits for multiplayer
	// or any other player!!!!
	if ( enterScreen )
		delete enterScreen;
	if ( exitScreen )
		delete exitScreen;

	enterScreen = exitScreen = NULL;
	enterScreen = new LoadScreen;
	exitScreen = new LoadScreen;
	bFirstTime = 0;
	useExitScreen = false; // PORT: begin with the initialized 800x600 entrance screen.
}

LoadScreenWrapper::~LoadScreenWrapper()
{
	if ( enterScreen )
		delete enterScreen;
	if ( exitScreen )
		delete exitScreen;

	enterScreen = exitScreen = NULL;
}

void LoadScreenWrapper::init( FitIniFile& file )
{
	enterScreen->init( file );

//	changeRes();
	bFirstTime = 0;
}

void LoadScreenWrapper::changeRes()
{
	const char* Appendix = NULL;

	// PORT: an active virtual canvas always uses the authored 800x600 loading
	// screen. Selecting a stock-resolution layout here would scale it as though
	// it were 800 wide and clip the right/bottom edges at widescreen.
	if (!MC2_IsVirtualCanvasActive())
	{
		// PORT: modern saved modes use Resolution=5 plus exact dimensions.
		// Retail loading art only exists for the authored 4:3 tiers, so choose
		// the largest fitting tier instead of asserting during options accept.
		if (prefs.resolution >= 5)
		{
			if (prefs.screenWidth >= 1600 && prefs.screenHeight >= 1200)
				Appendix = "_1600";
			else if (prefs.screenWidth >= 1280 && prefs.screenHeight >= 1024)
				Appendix = "_1280";
			else if (prefs.screenWidth >= 1024 && prefs.screenHeight >= 768)
				Appendix = "_1024";
			else if (prefs.screenWidth < 800 || prefs.screenHeight < 600)
				Appendix = "_640";
		}
		else
		{
			switch( prefs.resolution )
			{
			case 0:
				Appendix = "_640";
				break;

			case 1:
				break;

			case 2:
				Appendix = "_1024";

				break;

			case 3:
				Appendix = "_1280";

				break;

			case 4:
				Appendix = "_1600";

				break;

			default:
				break;
			}
		}
	}

	char fileName[256];
	sprintf( fileName, "mcl_loadingscreen" );

	if ( Appendix )
		strcat( fileName, Appendix );

	MC2_SelectWideLoadScreen( fileName );

	FullPathFileName path;

	path.init( artPath, fileName, ".fit" );

	FitIniFile outFile;

	if ( NO_ERR != outFile.open( path ) )
	{
		char error[256];
		sprintf( error, "couldn't open file %s", (char*)path );
		Assert( 0, 0, error );
		return;
	}

	//The 0x2 means that we do NOT want to flush this texture when we toss
	// the texture cache before a mission.  BUT we DO want the texture to cache out
	// to make more room for stuff.
	exitScreen->init( outFile, 0x2 );

	exitScreen->setupOutAnims();

	LoadScreen::changeRes( outFile );
}

void LoadScreen::changeRes( FitIniFile& outFile )
{

	if ( progressBackground )
		delete [] progressBackground;

	progressBackground = NULL;

	if ( waitingForPlayersMemory )
		delete [] waitingForPlayersMemory;

	waitingForPlayersMemory = NULL;

	if ( !progressBackground )
	{
		char progressPath[256];
		char progressBackgroundPath[256];



		long result = outFile.seekBlock( "LoadingBar" );
		gosASSERT( result == NO_ERR );

		outFile.readIdLong( "XLocation", xProgressLoc );
		outFile.readIdLong( "YLocation", yProgressLoc );
		outFile.readIdString( "FileName", progressPath, 255 );
		outFile.readIdString( "BackgroundFileName", progressBackgroundPath, 255);


		File tgaFile;
		FullPathFileName path;
		path.init( artPath, progressBackgroundPath, ".tga" );
		if ( NO_ERR != tgaFile.open( path ) )
		{
			char error[256];
			sprintf( error, "couldn't open file %s", (char*)path );
			Assert( 0, 0, error );
			return;
		}

		long size = tgaFile.fileSize();
	
		progressBackground = (TGAFileHeader*)new char[size];
		tgaFile.read( (unsigned char*)progressBackground, tgaFile.fileSize() );

		tgaFile.close();

		path.init( artPath, progressPath, ".tga" );
		if ( NO_ERR != tgaFile.open( path ) )
		{
			char error[256];
			sprintf( error, "couldn't open file %s", (char*)path );
			Assert( 0, 0, error );
			return;
		}

		size = tgaFile.fileSize();

		progressTextureMemory = (TGAFileHeader*)new char[size];
		tgaFile.read( (unsigned char*)progressTextureMemory, tgaFile.fileSize() );

		mergedTexture = (TGAFileHeader*)new char[size];
		memcpy( mergedTexture, progressTextureMemory, size );

		// PORT (M8-B49): the same two images, as GUI quads this time.
		if ( NO_ERR == outFile.seekBlock( "LoadingBar" ) )
		{
			long bx = 0, by = 0, bw = 0, bh = 0, bu = 0, bv = 0;
			outFile.readIdLong( "XLocation", bx );
			outFile.readIdLong( "YLocation", by );
			outFile.readIdLong( "Width", bw );
			outFile.readIdLong( "Height", bh );
			outFile.readIdLong( "UWidth", bu );
			outFile.readIdLong( "VHeight", bv );

			char fillName[256] = "", backName[256] = "";
			outFile.readIdString( "FileName", fillName, 255 );
			outFile.readIdString( "BackgroundFileName", backName, 255 );

			if ( !progressBarBack ) progressBarBack = new aObject;
			if ( !progressBarFill ) progressBarFill = new aObject;

			char buffer[256];
			strcpy( buffer, artPath );
			strcat( buffer, backName );
			_strlwr( buffer );
			if ( !strstr( buffer, ".tga" ) ) strcat( buffer, ".tga" );
			progressBarBack->init( bx, by, bw, bh );
			progressBarBack->setTexture( buffer );
			MC2_SetBarUVs( progressBarBack, 0, 0, bu, bv );
			// aObject::init(x,y,w,h) sets every vertex argb to 0xff000000 --
			// BLACK -- while the FitIniFile init uses 0xffffffff. The vertex
			// colour multiplies the texture, so a quad built from a rect draws
			// black no matter what is on it. That is why this bar was drawn,
			// visible and showing, and still could not be seen.
			progressBarBack->setColor( 0xffffffff );

			strcpy( buffer, artPath );
			strcat( buffer, fillName );
			_strlwr( buffer );
			if ( !strstr( buffer, ".tga" ) ) strcat( buffer, ".tga" );
			progressBarFill->init( bx, by, bw, bh );
			progressBarFill->setTexture( buffer );
			MC2_SetBarUVs( progressBarFill, 0, 0, bu, bv );
			// aObject::init(x,y,w,h) sets every vertex argb to 0xff000000 --
			// BLACK -- while the FitIniFile init uses 0xffffffff. The vertex
			// colour multiplies the texture, so a quad built from a rect draws
			// black no matter what is on it. That is why this bar was drawn,
			// visible and showing, and still could not be seen.
			progressBarFill->setColor( 0xffffffff );

			progressBarW = bw; progressBarH = bh;
			progressBarU = bu; progressBarV = bv;

			// PORT (M8-B49): report what was actually built. The first attempt at this
			// bar changed nothing on screen, so the next run has to come back with the
			// state rather than another theory. Says it once, always on.
			{
				static bool said = false;
				if ( !said )
				{
					said = true;
					fprintf( stderr, "[loadbar] built rect=(%ld,%ld) %ldx%ld uv=%ldx%ld fill='%s' back='%s' handles fill=%lu back=%lu argb=0x%08lx\n",
						bx, by, bw, bh, bu, bv, fillName, backName,
						(unsigned long)progressBarFill->getTextureHandle(),
						(unsigned long)progressBarBack->getTextureHandle(),
						(unsigned long)progressBarFill->getColor() );
				}
			}

		}


		tgaFile.close();

		result = outFile.seekBlock( "WaitImage" );
		gosASSERT( result == NO_ERR );
		outFile.readIdString( "FileName", progressPath, 255 );
		outFile.readIdLong( "XLocation", xWaitLoc );
		outFile.readIdLong( "YLocation", yWaitLoc );

		path.init( artPath, progressPath, ".tga" );
		if ( NO_ERR != tgaFile.open( path ) )
		{
			char error[256];
			sprintf( error, "couldn't open file %s", (char*)path );
			Assert( 0, 0, error );
			return;
		}

		size = tgaFile.fileSize();

		waitingForPlayersMemory = (TGAFileHeader*)new char[size];
		tgaFile.read( (unsigned char*)waitingForPlayersMemory, tgaFile.fileSize() );

		flipTopToBottom( (BYTE*)(waitingForPlayersMemory + 1), waitingForPlayersMemory->pixel_depth, 
			waitingForPlayersMemory->width, waitingForPlayersMemory->height );
	}
}

void LoadScreenWrapper::begin()
{
	waitForResChange = 0;
	bFirstTime = true;
	useExitScreen = false; // PORT: physical width no longer identifies this phase.

	enterScreen->begin();
}

void LoadScreenWrapper::update()
{
	if ( loadProgress > 99 )
		soundSystem->playDigitalSample( LOAD_DOORS_OPENING );
	else if ( bFirstTime )
		soundSystem->playDigitalSample( LOAD_DOORS_CLOSING );

	 bFirstTime = 0;

	if ( waitForResChange ) // waiting one more render to force 
	{
		status = READYTOLOAD;
		waitForResChange = 0;
	}
	else
	{

	if ( !useExitScreen ) // PORT: widescreen keeps the physical width throughout.
		{
			enterScreen->update();
			status = enterScreen->getStatus();
			if ( status == READYTOLOAD )
			{
				waitForResChange = 1;
				changeRes();
				useExitScreen = true; // PORT: changeRes initialized exitScreen and its anims.
				status = RUNNING;
			}
		}
		else
		{
			exitScreen->update();
			status = exitScreen->getStatus();
		}
	}
}

void LoadScreenWrapper::render( int xOffset, int yOffset )
{
	if ( !useExitScreen ) // PORT: render only the screen initialized for this phase.
	{
		enterScreen->render( xOffset, yOffset );
	}
	else
		exitScreen->render( xOffset, yOffset );
}


//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
LoadScreen::LoadScreen(  )
{
	progressBackground = 0;
	progressTextureMemory = 0;
	animIndices = 0;
}

//-------------------------------------------------------------------------------------------------

LoadScreen::~LoadScreen()
{
	if ( progressBackground )
		delete [] progressBackground;

	if ( progressTextureMemory )
		delete [] progressTextureMemory;

	if ( mergedTexture )
		delete [] mergedTexture;

	if ( animIndices )
		delete [] animIndices;

	if ( waitingForPlayersMemory)
		delete [] waitingForPlayersMemory;

	animIndices = NULL;

	waitingForPlayersMemory = progressBackground = progressTextureMemory = mergedTexture = NULL;
}

void LoadScreen::begin()
{
	for ( int i = 0; i < animObjectsCount; i++ )
	{
		//The assign below will overwrite the assign in defaultConstructor, leaking the memory
		animObjects[i].animInfo.destroy();

		animObjects[i].animInfo = inAnims[animIndices[i]];
		animObjects[i].animInfo.begin();
		animObjects[i].update();
	}

	loadProgress = 0;

	//-----------------------------------------------
	//Turn the mouse cursor OFF until load is done.
	// This will be keep ghost images from occuring.
	userInput->mouseOff();	
}
void LoadScreen::init(FitIniFile& file, DWORD neverFlush)
{
	LogisticsScreen::init( file, "Static", "Text", "Rect", "Button", "Edit", "AnimObject", neverFlush );

	file.seekBlock( "AnimationTopOut" );
	outAnims[0].init( &file, "" );

	file.seekBlock( "AnimationBottomOut" );
	outAnims[1].init( &file, "" );

	file.seekBlock( "AnimationLeftOut" );
	outAnims[2].init( &file, "" );

	file.seekBlock( "AnimationRightOut" );
	outAnims[3].init( &file, "" );

	file.seekBlock( "AnimationTopIn" );
	inAnims[0].init( &file, "" );

	file.seekBlock( "AnimationBottomIn" );
	inAnims[1].init( &file, "" );

	file.seekBlock( "AnimationLeftIn" );
	inAnims[2].init( &file, "" );

	file.seekBlock( "AnimationRightIn" );
	inAnims[3].init( &file, "" );

	file.seekBlock( "AnimationTop2In" );
	inAnims[4].init( &file, "" );

	file.seekBlock( "AnimationTop2Out" );
	outAnims[4].init( &file, "" );


	text.init( &file, "AnimObject18" );
	

	if ( animObjectsCount )
	{
		char blockName[256];
		animIndices = new int[animObjectsCount];
		for ( int i= 0; i < animObjectsCount; i++ )
		{
			sprintf( blockName, "AnimObject%ld", i );
			file.seekBlock( blockName );
			file.readIdString( "AnimationOut", blockName, 255 );
			if ( strstr( blockName, "2" ) )
				animIndices[i] = 4;
			else if ( strstr( blockName, "Top" ) )
				animIndices[i] = 0;
			else if ( strstr( blockName, "Bottom" ) )
				animIndices[i] = 1;
			else if ( strstr( blockName, "Left" ) )
				animIndices[i] = 2;
			else
				animIndices[i] = 3;
		}
	}

}

void LoadScreen::update()
{
	status = RUNNING;
	LogisticsScreen::update();

	bool bDone = true;
	for ( int i = 0; i < animObjectsCount; i++ )
	{
		bDone &= animObjects[i].isDone();
	}

	if ( bDone )
	{
		if ( loadProgress < 99.f )
		{

			for ( int i = 0; i < animObjectsCount; i++ )
			{
				//The assign below will overwrite the assign in begin, leaking the memory
				animObjects[i].animInfo.destroy();

				animObjects[i].animInfo = outAnims[animIndices[i]];
				animObjects[i].begin();
				animObjects[i].update();
			}

			status = READYTOLOAD;
			prefs.applyPrefs( true );

			turnOffAsyncMouse = mc2UseAsyncMouse;
			if ( !mc2UseAsyncMouse )
				MouseTimerInit();
			mc2UseAsyncMouse = true;			
			AsynFunc = ProgressTimer;
		}
		else
		{
			loadProgress = 0;

			status = NEXT;

			//YIKES!!  We could be checking the if before the null and executing after!!  Block the thread!
			//Wait for thread to finish.
			while (mc2IsInMouseTimer)
				;

			//ONLY set the mouse BLT data at the end of each update.  NO MORE FLICKERING THEN!!!
			// BLOCK THREAD WHILE THIS IS HAPPENING
			mc2IsInDisplayBackBuffer = true;

			AsynFunc = NULL;
			mc2UseAsyncMouse = turnOffAsyncMouse;
			if ( !mc2UseAsyncMouse)
				MouseTimerKill();

			mc2IsInDisplayBackBuffer = false;

			//Force mouse Cursors to smaller or larger depending on new video mode.
			userInput->initMouseCursors("cursors");
			userInput->mouseOn();
		}
	}
	else
	{
		userInput->mouseOff();
	}
}

void LoadScreen::render( int x, int y )
{
	//ignoring animation information...
	LogisticsScreen::render();

	int curX = animObjects[0].animInfo.getXDelta();
	int curY = animObjects[0].animInfo.getYDelta();
	text.move( x + curX, y + curY );
	text.render( );
	text.move( -x - curX, -y - curY);

	// PORT (M8-B49): the bar, on top of the art and riding the same slide as the
	// readout above it. The filled quad is CLIPPED rather than stretched -- its
	// width and its U both scale with progress, so the bar fills instead of the
	// artwork smearing sideways.
	if ( progressBarBack && progressBarFill && progressBarW > 0 )
	{
		// PORT (M8-B49): and report what the draw actually sees, once.
		{
			static bool said = false;
			if ( !said && loadProgress > 0.f )
			{
				said = true;
				fprintf( stderr, "[loadbar] first draw: loadProgress=%.1f showing=%d backShown=%d fillShown=%d\n",
						loadProgress,
						(int)isShowing(), (int)progressBarBack->isShowing(),
						(int)progressBarFill->isShowing() );
			}
		}
		float frac = loadProgress * 0.01f;
		if ( frac < 0.f ) frac = 0.f;
		if ( frac > 1.f ) frac = 1.f;

		progressBarBack->move( x + curX, y + curY );
		progressBarBack->render();
		progressBarBack->move( -x - curX, -y - curY );

		const long filled = (long)(progressBarW * frac);
		if ( filled > 0 )
		{
			progressBarFill->resize( filled, progressBarH );
			MC2_SetBarUVs( progressBarFill, 0, 0, (long)( progressBarU * frac ), progressBarV );
			progressBarFill->move( x + curX, y + curY );
			progressBarFill->render();
			progressBarFill->move( -x - curX, -y - curY );
		}
	}
}


void ProgressTimer(	RECT& WinRect,DDSURFACEDESC2& mouseSurfaceDesc )
{

	if ( !LoadScreen::progressBackground )
		return;

	long destX = 0;
	long destY = 0;

	BYTE* pMem = (BYTE*)(LoadScreen::mergedTexture + 1);
	long destRight = 0;
	long destBottom = 0;

	long srcWidth = 0;
	long srcDepth = 0;


	if ( loadProgress > 0 && loadProgress < 100 )
	{
		destX = 0;
		destY = 0;

		long destWidth = LoadScreen::progressBackground->width;
		long destHeight = LoadScreen::progressBackground->height;

		float widthIncPerProgress = (float)destWidth * 0.01f;

		long* pLSrc =  (long*)(LoadScreen::progressBackground+1);
		long* pLDest = (long*)(LoadScreen::mergedTexture+1);

		// merge background and current progress together...
		for ( int i = 0; i < 2; i++ )
		{
			for ( int y = 0; y < destHeight; y++ )
			{

				for ( long x = 0; x < LoadScreen::progressBackground->width; x++ )
				{
					if ( x < destWidth )
						*pLDest++ = *pLSrc++;
					else
					{
						pLDest++;
						pLSrc++;
					}
				}
			}
			pLSrc = (long*)( LoadScreen::progressTextureMemory + 1 );
			pLDest = (long*)(LoadScreen::mergedTexture+1);

			destWidth = loadProgress * widthIncPerProgress;
		}

		destX = WinRect.left + (LoadScreen::xProgressLoc);
		destY = WinRect.top + (LoadScreen::yProgressLoc);

		pMem = (BYTE*)(LoadScreen::mergedTexture + 1);
		destRight = destX + LoadScreen::progressBackground->width;
		destBottom = (destY + LoadScreen::progressBackground->height);

		srcWidth = LoadScreen::progressBackground->width;
		srcDepth = LoadScreen::progressBackground->pixel_depth/8;
	}
	else if ( loadProgress == 1000 )
	{
		destX = WinRect.left + LoadScreen::xWaitLoc;
		destY = WinRect.top + LoadScreen::yWaitLoc;

		pMem = (BYTE*)(LoadScreen::waitingForPlayersMemory + 1);
		destRight = destX + LoadScreen::waitingForPlayersMemory->width;
		destBottom = (destY + LoadScreen::waitingForPlayersMemory->height);
		destRight = destRight > WinRect.right ? WinRect.right : destRight;
		destBottom = destBottom > WinRect.bottom ? WinRect.top : destBottom;


		srcWidth = LoadScreen::waitingForPlayersMemory->width;
		srcDepth = LoadScreen::waitingForPlayersMemory->pixel_depth/8;

	}
	else
		return;

		// now put it on the screen...

		long destWidth = destRight - destX;
		long destHeight = destBottom - destY;


		for ( int y = 0; y < destHeight; y++ )
		{
			BYTE* pSrc = pMem + y * srcWidth * srcDepth;
			BYTE* pDest = (MemoryPtr)mouseSurfaceDesc.lpSurface + destX * mouseSurfaceDesc.ddpfPixelFormat.dwRGBBitCount/8 + 										
										((destY + y) * mouseSurfaceDesc.lPitch);

			for ( long x = 0; x < destWidth; x++ )
			{

					DWORD mColor = *(long*)pSrc;
					BYTE baseAlpha 		= 0;
					BYTE baseColorRed	= (mColor & 0x00ff0000)>>16;
					BYTE baseColorGreen	= (mColor & 0x0000ff00)>>8;
					BYTE baseColorBlue 	= (mColor & 0x000000ff);
					pSrc += 4;

					if ( mouseSurfaceDesc.ddpfPixelFormat.dwRGBBitCount == 32 )
					{
						(*(long*)pDest) = mColor;
						pDest += 4;
					}
					else if ( mouseSurfaceDesc.ddpfPixelFormat.dwRGBBitCount == 24 )
					{
						
						if ( !baseAlpha )
						{
							pDest++;
							pDest++;
							pDest++; 
						}

						*pDest++ = baseColorRed;
						*pDest++ = baseColorGreen;
						*pDest++ = baseColorBlue;

					}
					else if ( mouseSurfaceDesc.ddpfPixelFormat.dwRGBBitCount == 16 )
					{
						bool in555Mode = false;
						if (GetNumberOfBits(mouseSurfaceDesc.ddpfPixelFormat.dwGBitMask) == 5)
							in555Mode = true;

						if ( !baseAlpha )
						{
							long clr;
							if (in555Mode)
							{
								clr = (baseColorRed >> 3) << 10;
								clr += (baseColorGreen >> 3) << 5;
								clr += (baseColorBlue >> 3);
							}
							else
							{
								clr = (baseColorRed >> 3) << 11;
								clr += (baseColorGreen >> 2) << 5;
								clr += (baseColorBlue >> 3);
							}
							
							*pDest++ = clr & 0xff;
							*pDest++ = clr >> 8;
						}
						else
						{
							pDest++;
							pDest++;
						}
					}


				}
			}
}


void LoadScreen::setupOutAnims()
{
	for ( int i = 0; i < animObjectsCount; i++ )
	{
		//The assign below will overwrite the assign in begin, leaking the memory
		animObjects[i].animInfo.destroy();

		animObjects[i].animInfo = outAnims[animIndices[i]];
		animObjects[i].begin();
		animObjects[i].update();
	}
}

//*************************************************************************************************
// end of file ( LoadScreen.cpp ) 
