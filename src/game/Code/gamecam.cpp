//---------------------------------------------------------------------------
//
// GameCam.h -- File contains the Game camera class definitions
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include Files
#ifndef GAMECAM_H
#include "gamecam.h"
#endif

#ifndef OBJMGR_H
#include "objmgr.h"
#endif

#ifndef MOVER_H
#include "mover.h"
#endif

#ifndef MISSION_H
#include "mission.h"
#endif

#ifndef TEAM_H
#include "Team.h" // PORT: match case-sensitive host path.
#endif

#ifndef COMNDR_H
#include "comndr.h"
#endif

#ifndef WEATHER_H
#include "weather.h"
#endif

#include <MLR/MLR.hpp> // PORT: use host-independent include separators.

//---------------------------------------------------------------------------
CameraPtr eye = NULL;

extern bool useShadows;
extern bool useFog;
extern bool DisplayCameraAngle;

extern MidLevelRenderer::MLRClipper * theClipper;

#define MAX_SHADOW_PITCH_CHANGE	(5.0f)

extern bool drawOldWay;

extern bool useNonWeaponEffects;
GenericAppearance *theSky = NULL;
//---------------------------------------------------------------------------
void GameCamera::destroy (void)
{
	if (theSky)
	{
		delete theSky;
		theSky = NULL;
	}

	if (compass)
	{
		delete compass;
		compass = NULL;
	}

	Camera::destroy();
}

//---------------------------------------------------------------------------
#include "mc2_render_split.h"   // PORT (M8-B18)

void GameCamera::render (void)
{
	//------------------------------------------------------
	// At present, these actually draw.  Later they will 
	// add elements to the draw list and sort and draw.
	// The later time has arrived.  We begin sorting immediately.
	// NO LONGER NEED TO SORT!
	// ZBuffer time has arrived.  Share and Enjoy!
	// Everything SIMPLY draws at the execution point into the zBuffer
	// at the correct depth.  Miracles occur at that point!
	// Big code change but it removes a WHOLE bunch of code and memory!
	
	//--------------------------------------------------------
	// Get new viewport values to scale stuff.  No longer uses
	// VFX stuff for this.  ALL GOS NOW!
	gos_GetViewport(&viewMulX, &viewMulY, &viewAddX, &viewAddY);

	MidLevelRenderer::MLRState default_state;
	default_state.SetBackFaceOn();
	default_state.SetDitherOn();
	default_state.SetTextureCorrectionOn();
	default_state.SetZBufferCompareOn();
	default_state.SetZBufferWriteOn();

	default_state.SetFilterMode(MidLevelRenderer::MLRState::BiLinearFilterMode);

	float z = 1.0f;
	Stuff::RGBAColor fColor;
	fColor.red = ((fogColor >> 16) & 0xff);
	fColor.green = ((fogColor >> 8) & 0xff);
	fColor.blue = ((fogColor) & 0xff);

	//--------------------------------------------------------
	// Get new viewport values to scale stuff.  No longer uses
	// VFX stuff for this.  ALL GOS NOW!
	screenResolution.x = viewMulX;
	screenResolution.y = viewMulY;
	calculateProjectionConstants();

	TG_Shape::SetViewport(viewMulX,viewMulY,viewAddX,viewAddY);

	userInput->setViewport(viewMulX,viewMulY,viewAddX,viewAddY);

	gos_TextSetRegion(viewAddX,viewAddY,viewMulX,viewMulY);
	//--------------------------------------------------------
	// Get new viewport values to scale stuff.  No longer uses
	// VFX stuff for this.  ALL GOS NOW!
	screenResolution.x = viewMulX;
	screenResolution.y = viewMulY;
	calculateProjectionConstants();

	globalScaleFactor = getScaleFactor();
	// PORT: widescreen -- scale off the LOCKED axis. The projection pins the
	// vertical extent, so keying unit size to width made units grow as the
	// window widened while the world did not. Identical at every 4:3 mode
	// (800/640 and 600/480 are both 1.25), so stock behaviour is unchanged.
	globalScaleFactor *= viewMulY / 480.0;		//Scale Mechs to ScreenRES
	
	//-----------------------------------------------
	// Set Ambient for this pass of rendering	
	DWORD lightRGB = (ambientRed<<16)+(ambientGreen<<8)+ambientBlue;
		
	eye->setLightColor(1,lightRGB);
	eye->setLightIntensity(1,1.0);

	MidLevelRenderer::PerspectiveMode = usePerspective;
	theClipper->StartDraw(cameraOrigin, cameraToClip, fColor, &fColor, default_state, &z);
	MidLevelRenderer::GOSVertex::farClipReciprocal = (1.0f-cameraToClip(2, 2))/cameraToClip(3, 2);

	if (active && turn > 1)
	{
		//----------------------------------------------------------
		// Turn stuff on line by line until perspective is working.
		
		if (Environment.Renderer != 3)
			MC2_RENDER_PHASE(MC2_RP_SKY, theSky->render(1));
		
		MC2_RENDER_PHASE(MC2_RP_LAND, land->render());	//render the Terrain

		if (Environment.Renderer != 3)
			MC2_RENDER_PHASE(MC2_RP_CRATERS, craterManager->render());
		
		MC2_RENDER_PHASE(MC2_RP_OBJECTS,
			ObjectManager->render(true, true, true));	//render all other objects

		MC2_RENDER_PHASE(MC2_RP_WATER, land->renderWater());	//Draw Water Last!

		if (useShadows && Environment.Renderer != 3)
			MC2_RENDER_PHASE(MC2_RP_SHADOWS,
				ObjectManager->renderShadows(true, true, true));

		if (mission && mission->missionInterface)
			mission->missionInterface->drawVTOL();
			
		if (!drawOldWay && !inMovieMode)
		{
			if (compass && (turn > 3) && drawCompass)
				MC2_RENDER_PHASE(MC2_RP_COMPASS, compass->render(-1));
		}
	
		if (!drawOldWay)
			// This is the one that actually sends triangles to the GPU; every
			// call above it only builds the lists.
			MC2_RENDER_PHASE(MC2_RP_RENDERLISTS, mcTextureManager->renderLists());

		if (drawOldWay)
		{
			//Last thing drawn were shadows which are not Gouraud Shaded!!!
			// MLR to be "efficient" doesn't set this state by default at startup!
			gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		}

		theClipper->RenderNow();		//Draw the FX


		if (useNonWeaponEffects)
			weather->render();				//Draw the weather
	}

	if (drawOldWay && !inMovieMode)
	{
		gos_SetRenderState( gos_State_ZCompare, 0);
		gos_SetRenderState(	gos_State_ZWrite, 0);
		gos_SetRenderState( gos_State_Perspective, 1);

		if (compass && (turn > 3) && drawCompass)
			compass->render();
	}
	
	//---------------------------------------------------------	
	//Check if we are inMovieMode and should be letterboxed.
	// draw letterboxes here.
	if (inMovieMode && (letterBoxPos != 0.0f))
	{
		//Figure out the two faces we need to draw based on letterBox Pos and Alpha
		float barTopX = screenResolution.y * letterBoxPos;
		float barBotX = screenResolution.y - barTopX;

		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_MonoEnable, 0);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Clipping, 1);
		gos_SetRenderState( gos_State_AlphaTest, 1);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 1);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterNone);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
		gos_SetRenderState( gos_State_ZCompare, 0);
		gos_SetRenderState(	gos_State_ZWrite, 0);
		gos_SetRenderState( gos_State_Texture, 0);
		
 		//------------------------------------
		gos_VERTEX gVertex[4];

		gVertex[0].x		= 0.0f;
		gVertex[0].y		= 0.0f;
		gVertex[0].z		= 0.00001f;
		gVertex[0].rhw		= 0.00001f;
		gVertex[0].u		= 0.0f;
		gVertex[0].v		= 0.0f;
		gVertex[0].argb		= (letterBoxAlpha << 24);
		gVertex[0].frgb		= 0xff000000;

		gVertex[1].x		= 0.0f;
		gVertex[1].y		= barTopX;
		gVertex[1].z		= 0.00001f;               
		gVertex[1].rhw		= 0.00001f;               
		gVertex[1].u		= 0.0f;                  
		gVertex[1].v		= 0.0f;                  
		gVertex[1].argb		= (letterBoxAlpha << 24);
		gVertex[1].frgb		= 0xff000000;            

		gVertex[2].x		= screenResolution.x;
		gVertex[2].y		= barTopX; 
		gVertex[2].z		= 0.00001f;               
		gVertex[2].rhw		= 0.00001f;               
		gVertex[2].u		= 0.0f;                  
		gVertex[2].v		= 0.0f;                  
		gVertex[2].argb		= (letterBoxAlpha << 24);
		gVertex[2].frgb		= 0xff000000;            

		gVertex[3].x		= screenResolution.x;
		gVertex[3].y		= 0.0f;
		gVertex[3].z		= 0.00001f;               
		gVertex[3].rhw		= 0.00001f;               
		gVertex[3].u		= 0.0f;                  
		gVertex[3].v		= 0.0f;                  
		gVertex[3].argb		= (letterBoxAlpha << 24);
		gVertex[3].frgb		= 0xff000000;            
		
		gos_DrawQuads(gVertex, 4);
		
		gVertex[0].x		= 0.0f;
		gVertex[0].y		= barBotX;
		gVertex[0].z		= 0.00001f;
		gVertex[0].rhw		= 0.00001f;
		gVertex[0].u		= 0.0f;
		gVertex[0].v		= 0.0f;
		gVertex[0].argb		= (letterBoxAlpha << 24);
		gVertex[0].frgb		= 0xff000000;

		gVertex[1].x		= screenResolution.x;
		gVertex[1].y		= barBotX;
		gVertex[1].z		= 0.00001f;               
		gVertex[1].rhw		= 0.00001f;               
		gVertex[1].u		= 0.0f;                  
		gVertex[1].v		= 0.0f;                  
		gVertex[1].argb		= (letterBoxAlpha << 24);
		gVertex[1].frgb		= 0xff000000;            

		gVertex[2].x		= screenResolution.x;
		gVertex[2].y		= screenResolution.y; 
		gVertex[2].z		= 0.00001f;               
		gVertex[2].rhw		= 0.00001f;               
		gVertex[2].u		= 0.0f;                  
		gVertex[2].v		= 0.0f;                  
		gVertex[2].argb		= (letterBoxAlpha << 24);
		gVertex[2].frgb		= 0xff000000;            

		gVertex[3].x		= 0.0f; 
		gVertex[3].y		= screenResolution.y;
		gVertex[3].z		= 0.00001f;               
		gVertex[3].rhw		= 0.00001f;               
		gVertex[3].u		= 0.0f;                  
		gVertex[3].v		= 0.0f;                  
		gVertex[3].argb		= (letterBoxAlpha << 24);
		gVertex[3].frgb		= 0xff000000;            
		
		gos_DrawQuads(gVertex, 4);
	}

	if (inMovieMode && (fadeAlpha != 0x0))
	{
		//We are fading to something other then clear screen.
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
		gos_SetRenderState( gos_State_MonoEnable, 0);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Clipping, 1);
		gos_SetRenderState( gos_State_AlphaTest, 1);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 1);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterNone);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureClamp );
		gos_SetRenderState( gos_State_ZCompare, 0);
		gos_SetRenderState(	gos_State_ZWrite, 0);
		gos_SetRenderState( gos_State_Texture, 0);
		
 		//------------------------------------
		gos_VERTEX gVertex[4];

		gVertex[0].x		= 0.0f;
		gVertex[0].y		= 0.0f;
		gVertex[0].z		= 0.00001f;
		gVertex[0].rhw		= 0.00001f;
		gVertex[0].u		= 0.0f;
		gVertex[0].v		= 0.0f;
		gVertex[0].argb		= (fadeAlpha << 24) + (fadeColor & 0x00ffffff);
		gVertex[0].frgb		= 0xff000000;

		gVertex[1].x		= 0.0f;
		gVertex[1].y		= screenResolution.y;
		gVertex[1].z		= 0.00001f;               
		gVertex[1].rhw		= 0.00001f;               
		gVertex[1].u		= 0.0f;                  
		gVertex[1].v		= 0.0f;                  
		gVertex[1].argb		= (fadeAlpha << 24) + (fadeColor & 0x00ffffff);
		gVertex[1].frgb		= 0xff000000;            

		gVertex[2].x		= screenResolution.x;
		gVertex[2].y		= screenResolution.y; 
		gVertex[2].z		= 0.00001f;               
		gVertex[2].rhw		= 0.00001f;               
		gVertex[2].u		= 0.0f;                  
		gVertex[2].v		= 0.0f;                  
		gVertex[2].argb		= (fadeAlpha << 24) + (fadeColor & 0x00ffffff);
		gVertex[2].frgb		= 0xff000000;            

		gVertex[3].x		= screenResolution.x;
		gVertex[3].y		= 0.0f;
		gVertex[3].z		= 0.00001f;               
		gVertex[3].rhw		= 0.00001f;               
		gVertex[3].u		= 0.0f;                  
		gVertex[3].v		= 0.0f;                  
		gVertex[3].argb		= (fadeAlpha << 24) + (fadeColor & 0x00ffffff);
		gVertex[3].frgb		= 0xff000000;            
		
		gos_DrawQuads(gVertex, 4);
	}
	
	//-----------------------------------------------------
}	

//---------------------------------------------------------------------------
// PORT: the two altitude clamps in GameCamera::update write cameraAltitude
// without touching cameraAltitudeDesired, so the tracer in Camera.cpp never
// sees them. Clamp B can RAISE altitude to exactly AltitudeMaximumLo, which is
// the 1500 measured at the snap, so it has to report for itself.
static void gameCamAltTrace(const char* where, float alt, float testMax, float desired)
{
    static int on = -1;
    if (on < 0) {
        const char* want = getenv("MC2_CAMERA_CENSUS");
        on = (want && *want && strcmp(want, "0")) ? 1 : 0;
    }
    if (!on) return;
    if (alt == testMax) return;   // fires every frame once settled; only report changes
    fprintf(stderr, "[cam-alt] %-22s alt %.1f -> %.1f   desired %.1f\n",
            where, alt, testMax, desired);
    fflush(stderr);
}

long GameCamera::activate (void)
{
	// PORT: which call actually aims the camera? (owner report 2026-08-10)
	//
	// The frame trace showed the camera sitting on class defaults -- altitude
	// 1200 (AltitudeDefault), scale 0.5795 -- panning slowly for 432 frames,
	// then snapping back to its frame-1 position with altitude 1500
	// (AltitudeMaximumLo) and scale 0.3824. That is what the owner sees. It does
	// NOT say what moved it, and the early return below means that CALLING
	// activate() is not the same as aiming.
	//
	// So report both outcomes. A run with no "aimed" line at the jump frame
	// rules this function out entirely, which is worth as much as a hit.
	const char* wantCamTrace = getenv("MC2_CAMERA_CENSUS");
	const bool camTraceOn = wantCamTrace && *wantCamTrace && strcmp(wantCamTrace, "0");

	//------------------------------------------
	// If camera is already active, just return
	if (ready && active)
	{
		if (camTraceOn)
		{
			fprintf(stderr, "[cam-event] activate: EARLY RETURN (ready=%d active=%d) -- camera NOT aimed\n",
					ready ? 1 : 0, active ? 1 : 0);
			fflush(stderr);
		}
		return(NO_ERR);
	}

	if (camTraceOn)
	{
		fprintf(stderr, "[cam-event] activate: proceeding (ready=%d active=%d)\n",
				ready ? 1 : 0, active ? 1 : 0);
		fflush(stderr);
	}

	//---------------------------------------------------------
	// Camera always starts pointing at first mover in lists
	// CANNOT be infinite because we don't allow missions without at least 1 player mech!!
	MoverPtr firstMover = NULL;
	if (ObjectManager->getNumMovers() > 0) {
		long i = 0;
		firstMover = ObjectManager->getMover(i);
		while (firstMover && ((firstMover->getCommander()->getId() != Commander::home->getId()) || !firstMover->isOnGUI()))
		{
			i++;
			if (i == ObjectManager->getNumMovers())
				break;
			firstMover = ObjectManager->getMover(i); 
		}
	}
	
	if (firstMover)
	{
		Stuff::Vector3D newPosition(firstMover->getPosition());
		setPosition(newPosition);

		if (camTraceOn)
		{
			fprintf(stderr, "[cam-event] activate: AIMED at (%.1f,%.1f,%.1f) alt=%.1f desired=%.1f\n",
					newPosition.x, newPosition.y, newPosition.z,
					cameraAltitude, cameraAltitudeDesired);
			fflush(stderr);
		}
	}
	else if (camTraceOn)
	{
		fprintf(stderr, "[cam-event] activate: NO FIRST MOVER -- camera left where it was\n");
		fflush(stderr);
	}

	if (land)
	{
		land->update();
	}
		
	allNormal();
	
	//updateDaylight(true);
	
	lastShadowLightPitch = lightPitch;
	
	//Startup the SKYBox
	long appearanceType = (GENERIC_APPR_TYPE << 24);

	AppearanceTypePtr genericAppearanceType = NULL;
	genericAppearanceType = appearanceTypeList->getAppearance(appearanceType,"skybox");
	if (!genericAppearanceType)
	{
		char msg[1024];
		sprintf(msg,"No Generic Appearance Named %s","skybox");
		Fatal(0,msg);
	}
	  
   	theSky = new GenericAppearance;
	gosASSERT(theSky != NULL);

	//--------------------------------------------------------------
	gosASSERT(genericAppearanceType->getAppearanceClass() == GENERIC_APPR_TYPE);
	theSky->init((GenericAppearanceType*)genericAppearanceType, NULL);
	
	theSky->setSkyNumber(mission->theSkyNumber);
			
 	return NO_ERR;
}

inline GameObjectPtr getCamObject (long partId, bool existsOnly) 
{
	GameObjectPtr obj = NULL;
	if (partId == -1)
		obj = NULL;
	else
		obj = ObjectManager->findByPartId(partId);

	if (existsOnly) 
	{
		if (obj && 
			obj->getExists() && 
			(obj->getCommanderId() == Commander::home->getId()) || 
			(Team::home->teamLineOfSight(obj->getLOSPosition(),0.0f)))
			return(obj);
		return(NULL);
	}

	return(obj);
}

long cameraLineChanged = 0;
bool useLOSAngle = true;
//---------------------------------------------------------------------------
long GameCamera::update (void)
{
	if (lookTargetObject != -1)
		targetObject = getCamObject(lookTargetObject,true);
		
	if (targetObject && 
		targetObject->getExists() && 
		((targetObject->getCommanderId() == Commander::home->getId()) || 
		!targetObject->isMover() ||
		(targetObject->isMover() && ((Mover *)targetObject)->conStat >= CONTACT_SENSOR_QUALITY_1) ))
	{
		setPosition(targetObject->getPosition(),false);
	}
	else
	{
		targetObject = NULL;
	}

	//Force CameraAltitude to be less than max based on angle.  This keeps poly load relatively even	
	float anglePercent = (projectionAngle - MIN_PERSPECTIVE) / (MAX_PERSPECTIVE - MIN_PERSPECTIVE);
	float testMax = Camera::AltitudeMaximumLo + ((Camera::AltitudeMaximumHi - Camera::AltitudeMaximumLo) * anglePercent);
	
	if (cameraAltitude > testMax)
	{
		// PORT: clamp A -- altitude above the angle's ceiling.
		gameCamAltTrace("update: clamp>testMax", cameraAltitude, testMax, cameraAltitudeDesired);
		cameraAltitude = testMax;
	}

	if ((cameraAltitude < testMax) && (cameraAltitudeDesired > testMax))
	{
		// PORT: clamp B -- this is the one that can RAISE altitude to exactly
		// AltitudeMaximumLo, which is the 1500 the trace recorded at the snap.
		gameCamAltTrace("update: desired>testMax", cameraAltitude, testMax, cameraAltitudeDesired);
		cameraAltitude = testMax;
	}
												  
	// calculate new near and far plane distance based on 
	// Current altitude above terrain.
	float altitudePercent = (cameraAltitude - AltitudeMinimum) / (testMax - AltitudeMinimum);
	Camera::NearPlaneDistance = MinNearPlane + ((MaxNearPlane - MinNearPlane) * altitudePercent);
	Camera::FarPlaneDistance = MinFarPlane + ((MaxFarPlane - MinFarPlane) * altitudePercent);
	
	if (userInput->getKeyDown(KEY_LBRACKET) && userInput->ctrl() && userInput->alt() && !userInput->shift())
	{
		useLOSAngle ^= true;
	}		

#ifdef DEBUG_CAMERA
	if (userInput->getKeyDown(KEY_RBRACKET) && userInput->ctrl() && userInput->alt() && !userInput->shift())
	{
		Camera::NearPlaneDistance += 10.0f;
	}		

	if (userInput->getKeyDown(KEY_APOSTROPHE) && userInput->ctrl() && userInput->alt() && !userInput->shift())
	{
		Camera::FarPlaneDistance -= 1005.00f;
	}		

	if (userInput->getKeyDown(KEY_SEMICOLON) && userInput->ctrl() && userInput->alt() && !userInput->shift())
	{
		Camera::FarPlaneDistance += 1005.0f;
	}		

	char text[1024];
	sprintf(text,"Near Plane: %f     Far Plane: %f",Camera::NearPlaneDistance,Camera::FarPlaneDistance);

	DWORD width, height;
	Stuff::Vector4D moveHere;
	moveHere.x = 10.0f;
	moveHere.y = 10.0f;

	gos_TextSetAttributes (gosFontHandle, 0, gosFontScale, false, true, false, false);
	gos_TextStringLength(&width,&height,text);

	moveHere.z = width;
	moveHere.w = height;

	globalFloatHelp[currentFloatHelp].setHelpText(text);
	globalFloatHelp[currentFloatHelp].setScreenPos(moveHere);
	globalFloatHelp[currentFloatHelp].setForegroundColor(SD_GREEN);
	globalFloatHelp[currentFloatHelp].setBackgroundColor(SD_BLACK);
	globalFloatHelp[currentFloatHelp].setScale(1.0f);
	globalFloatHelp[currentFloatHelp].setProportional(true);
	globalFloatHelp[currentFloatHelp].setBold(false);
	globalFloatHelp[currentFloatHelp].setItalic(false);
	globalFloatHelp[currentFloatHelp].setWordWrap(false);

	currentFloatHelp++;

	gosASSERT(currentFloatHelp < MAX_FLOAT_HELPS);
#endif

	if (DisplayCameraAngle)
	{
		char text[1024];
		sprintf(text,"Camera Angle: %f degrees    Camera Altitude: %f    CameraPosition: X=%f Y=%f Z=%f   CameraRotation: %f",projectionAngle,cameraAltitude,position.x,position.y,position.z,cameraRotation);
		
		DWORD width, height;
		Stuff::Vector4D moveHere;
		moveHere.x = 10.0f;
		moveHere.y = 10.0f;

		gos_TextSetAttributes (gosFontHandle, 0, gosFontScale, false, true, false, false);
		gos_TextStringLength(&width,&height,text);

		moveHere.z = width;
		moveHere.w = height;

		globalFloatHelp->setFloatHelp(text,moveHere,SD_GREEN,SD_BLACK,1.0f,true,false,false,false);
	}

	if (!compass)	//Create it!
	{
		AppearanceType* appearanceType = appearanceTypeList->getAppearance( BLDG_TYPE << 24, "compass" );
		compass = new BldgAppearance;
		compass->init( appearanceType );
	}

	long result = Camera::update();
	
//	if ((day2NightTransitionTime > 0.0f) && !getIsNight() && (fabs(lastShadowLightPitch-lightPitch) > MAX_SHADOW_PITCH_CHANGE))
//	{
//		forceShadowRecalc = true;
//		lastShadowLightPitch = lightPitch;
//	}
//	else
//	{
//		forceShadowRecalc = false;
//	}
	
	//Always TRUE for right now.  Debugging....
	//-fs
	//forceShadowRecalc = true;
	
	bool oldFog = useFog;
	bool oldShadows = useShadows;
	useFog = false;
	useShadows = false;
		
  	if (compass && (turn > 3))
	{
  		
   		Stuff::Vector3D compassPos = getPosition();	// PORT: non-const ref cannot bind a temporary
   		compass->setObjectParameters(compassPos,0.0f,false,0,0);
   		compass->setMoverParameters(0.0f);
   		compass->setGesture(0);
   		compass->setObjStatus(OBJECT_STATUS_DESTROYED);
   		compass->setInView(true);
   		compass->setVisibility(true,true);
   		compass->setFilterState(true);
		compass->setIsHudElement();
   		compass->update();		   //Force it to try and draw or stuff will not work!
	}

	if (theSky)
	{
		Stuff::Vector3D pos = getPosition();


   		theSky->setObjectParameters(pos,0.0f,false,0,0);
   		theSky->setMoverParameters(0.0f);
   		theSky->setGesture(0);
   		theSky->setObjStatus(OBJECT_STATUS_NORMAL);
   		theSky->setInView(true);
   		theSky->setVisibility(true,true);
   		theSky->setFilterState(true);
		theSky->setIsHudElement();
   		theSky->update();		   //Force it to try and draw or stuff will not work!
	}
  
	useFog = oldFog;
	useShadows = oldShadows;
	
	return result;
}

//---------------------------------------------------------------------------
//
// Edit log
//
//---------------------------------------------------------------------------
