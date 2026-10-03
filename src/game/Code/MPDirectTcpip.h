#ifndef MPDIRECTTCPIP_H
#define MPDIRECTTCPIP_H
/*************************************************************************************************\
MPDirectTcpip.h			: Interface for the MPDirectTcpip component.
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

#ifndef AANIM_H
#include "AANIM.H" // PORT: match case-sensitive host path.
#endif

class aButton;


class aStyle7TextListItem : public aTextListItem
{
public:
	aStyle7TextListItem() { hasAnimation = false; normalColor = 0xff808080; }
	virtual long	init( FitIniFile* file, const char* blockName );
	virtual void		render();

protected:
	bool hasAnimation;
	aAnimGroup animGroup;
	int normalColor;
};

class MPDirectTcpip : public LogisticsScreen
{
public:
	
	MPDirectTcpip();
	virtual ~MPDirectTcpip();
	
	void init(FitIniFile* file);
	bool isDone();
	virtual void		begin();
	virtual void		end();
 	virtual void render( int xOffset, int yOffset );
	virtual void render();
	virtual void update();
	virtual int			handleMessage( unsigned long, unsigned long );

	bool				bDone;


private:
	int indexOfButtonWithID(int id);

	aComboBox				ipAddressComboBox;
};



//*************************************************************************************************
#endif  // end of file ( MPDirectTcpip.h )
