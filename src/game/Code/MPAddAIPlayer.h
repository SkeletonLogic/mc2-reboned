#ifndef MPADDAIPLAYER_H
#define MPADDAIPLAYER_H
/*************************************************************************************************\
MPAddAIPlayer.h			: Interface for the MPAddAIPlayer component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

//*************************************************************************************************

#ifndef LOGISTICSSCREEN_H
#include "LogisticsScreen.h"
#endif

#include "ASYSTEM.H" // PORT: match case-sensitive host path.
#include "ALISTBOX.H" // PORT: match case-sensitive host path.
#include "AttributeMeter.h"
#include "simpleCamera.h" // PORT: match case-sensitive host path.
#include "MPParameterScreen.h"/*only for the definition of CFocusManager*/

#ifndef AANIM_H
#include "AANIM.H" // PORT: match case-sensitive host path.
#endif


class aStyle4TextListItem : public aTextListItem
{
public:
	aStyle4TextListItem() { hasAnimation = false; normalColor = 0xff808080; }
	virtual long	init( FitIniFile* file, const char* blockName );
	virtual void		render();

protected:
	bool hasAnimation;
	aAnimGroup animGroup;
	int normalColor;
};

class MPAddAIPlayer : public LogisticsScreen
{
public:
	
	MPAddAIPlayer();
	virtual ~MPAddAIPlayer();
	
	void init(FitIniFile* file);
	bool isDone();
	virtual void		begin();
	virtual void		end();
 	virtual void render( int xOffset, int yOffset );
	virtual void render();
	virtual void update();
	virtual int			handleMessage( unsigned long, unsigned long );

private:
	aEdit		nameEntry;
	aRect		nameEntryOutline;
	aDropList				experienceDropList;
	aDropList				factionDropList;
	aDropList				mechSelectionDropLists[4][3];

	int indexOfButtonWithID(int id);

	CFocusManager focusManager;
};



//*************************************************************************************************
#endif  // end of file ( MPAddAIPlayer.h )
