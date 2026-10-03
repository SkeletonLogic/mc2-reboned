#ifndef SALVAGEMECHAREA_H
#define SALVAGEMECHAREA_H
/*************************************************************************************************\
SalvageMechArea.h			: Interface for the SalvageMechArea component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

//*************************************************************************************************
#include "ASYSTEM.H" // PORT: match case-sensitive host path.
#include "SimpleComponentListBox.h"
#include "AttributeMeter.h"
#include "LogisticsScreen.h"
#include "simpleCamera.h" // PORT: match case-sensitive host path.

class aButton;
class MechIcon;
class BattleMech;
class LogisticsVariant;

/**************************************************************************************************
CLASS DESCRIPTION
SalvageMechArea:
**************************************************************************************************/
class SalvageMechArea : public LogisticsScreen
{
	public:

	static SalvageMechArea* instance;
	SalvageMechArea();
	virtual ~SalvageMechArea();

	void init( FitIniFile* file );
	void setMech( LogisticsVariant* pMech, long red, long green, long blue );

	virtual void render(long xOffset, long yOffset);
	virtual void update();

	
	protected:

	LogisticsVariant*		unit;

	AttributeMeter		attributeMeters[3];
	ComponentListBox	loadoutListBox;
	SimpleCamera		mechCamera;

};


class SalvageMechScreen : public LogisticsScreen
{
public:
	
	SalvageMechScreen();
	virtual ~SalvageMechScreen();
	
	void init(FitIniFile* file);
	bool isDone();
	bool donePressed() { return bDone; }
	virtual void render();
	virtual void update();
	virtual int			handleMessage( unsigned long, unsigned long );

	void updateSalvage(); // put into inventory

	bool				bDone;


private:

	aListBox			salvageListBox;

	SalvageMechArea		selMechArea;
	aAnimation			exitAnim;
	aAnimation			entryAnim;

	float				countDownTime;
	float				curCount;
	float				previousAmount;
	float				oldCBillsAmount;
};

class SalvageListItem : public aListItem
{
public:
	virtual void		render();
	virtual void		update();

	virtual ~SalvageListItem();

	static void			init( FitIniFile* file );
	virtual int			handleMessage( unsigned long message, unsigned long who );

	bool				isChecked();

	// PORT (owner request 2026-09-10): every 10,000 resource points carried out of
	// a mission buys one FREE mech salvage. Finish under 10,000 and nothing is
	// free, exactly as before. The allowance is shared across the list, so it is a
	// static, and each item remembers whether IT was the one taken free so that
	// unchecking returns the right thing -- an allowance slot, not C-Bills.
	static long			freeSalvagesLeft;
	static long			freeSalvagesTotal;
	static void			setFreeSalvageAllowance( long resourcePoints );
	LogisticsVariant*	getMech(){ return pVariant; }

	SalvageListItem( BattleMech* pUnit );

private:
	static aAnimButton*	templateCheckButton;
	static aText*		mechNameText;
	static aText*		variantNameText;
	static aText*		weightText;
	static aText*		costText;
	static aObject*		cBillsIcon;
	static aObject*		weightIcon;
	static GUI_RECT		iconRect;
	static GUI_RECT		rect;
	static aAnimation*	s_pressedAnim;
	static aAnimation*	s_highlightAnim;
	static aAnimation*	s_normalAnim;

	aAnimation			pressedAnim;
	aAnimation			highlightAnim;
	aAnimation			normalAnim;

	int					salvageAmount;
	float				costToSalvage;
	MechIcon*			icon;
	LogisticsVariant*	pVariant;
	aAnimButton*		checkButton;

	bool				salvagedFree;

	unsigned long				psRed;
	unsigned long				psGreen;
	unsigned long				psBlue;

	friend class SalvageMechScreen;


	
};




//*************************************************************************************************
#endif  // end of file ( SalvageMechArea.h )
