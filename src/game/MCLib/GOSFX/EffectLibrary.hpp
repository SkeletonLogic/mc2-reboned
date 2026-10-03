//==========================================================================//
// File:	 EffectLibrary.hpp												//
// Contents: Base Effect Component											//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#pragma once

#include "gosFX.hpp"
#include "Effect.hpp"

namespace gosFX
{
#if defined(__EMSCRIPTEN__)
	// See EffectLibrary.cpp: with mc2.fx deferred, any non-NULL Effect* is stale
	// memory and dereferencing it traps and stops the browser main loop.
	void DropEffectWithoutLibrary(const char* which, Effect*& effect);
	// True only when MC2_BROWSER_EFFECTS is set. BOTH mc2.fx loaders must ask.
	bool BrowserEffectsEnabled();
#	define MC2_DROP_STALE_EFFECT(x) gosFX::DropEffectWithoutLibrary(#x, x)
#else
#	define MC2_DROP_STALE_EFFECT(x) ((void)0)
#endif
	class EffectLibrary
		#if defined(_ARMOR)
			: public Stuff::Signature
		#endif
	{
	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Initialization
	//
	public:
		static void
			InitializeClass();
		static void
			TerminateClass();

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Constructors/Destructors
	//
	public:
		EffectLibrary();
		~EffectLibrary();

		void
			Load(Stuff::MemoryStream* stream);
		void
			Save(Stuff::MemoryStream* stream);

		enum MergeMode {
			OnlyAddNewEffects,
			ReplaceMatchingEffects,
			ReplaceNamedEffects
		};

		void
			Merge(
				EffectLibrary &source,
				MergeMode merge_mode=(MergeMode)OnlyAddNewEffects
			);

		static EffectLibrary*
			Instance;

	protected:
		EffectLibrary(EffectLibrary &source)
			{STOP(("Shouldn't be called"));}

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Effect management
	//
	public:
		Stuff::DynamicArrayOf<Effect::Specification*>
			m_effects;

		Effect::Specification*
			Find(const char* name);
		Effect*
			MakeEffect(
				unsigned index,
				unsigned flags
			);

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Testing
	//
	public:
		void
			TestInstance() const
				{}
	};
}
