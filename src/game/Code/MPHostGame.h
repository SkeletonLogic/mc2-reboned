#ifndef MPHOSTGAME_H
#define MPHOSTGAME_H
/*************************************************************************************************\
MPHostGame.h			: Interface for the MPHostGame component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

//*************************************************************************************************

#ifndef LOGISTICSDIALOG_H
#include "LogisticsDialog.h"
#endif

#include "ASYSTEM.H" // PORT: match case-sensitive host path.
#include "ALISTBOX.H" // PORT: match case-sensitive host path.
#include "AttributeMeter.h"
#include "simpleCamera.h" // PORT: match case-sensitive host path.

#ifndef AANIM_H
#include "AANIM.H" // PORT: match case-sensitive host path.
#endif


class aStyle5TextListItem : public aTextListItem
{
public:
	aStyle5TextListItem() { hasAnimation = false; normalColor = 0xff808080; }
	virtual long	init( FitIniFile* file, const char* blockName );
	virtual void		render();

protected:
	bool hasAnimation;
	aAnimGroup animGroup;
	int normalColor;
};

class MPHostGame : public LogisticsDialog
{
public:
	
	MPHostGame();
	virtual ~MPHostGame();
	
	void init();
	bool isDone();
	virtual void		begin();
	virtual void		end();
 	virtual void render( int xOffset, int yOffset );
	virtual void render();
	virtual void update();
	virtual int			handleMessage( unsigned long, unsigned long );


private:
//	aEdit		nameEntry;
//	aRect		nameEntryOutline;
	bool					bShowDlg;

	int indexOfButtonWithID(int id);
};



//*************************************************************************************************
#endif  // end of file ( MPHostGame.h )
