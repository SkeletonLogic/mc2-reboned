#include "gosFXHeaders.hpp"

#if defined(__EMSCRIPTEN__)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <emscripten/emscripten.h>

static unsigned g_mc2EffectsMade = 0;

// PORT: MC2.FX is a shipped 1998 asset and its loader is not clean.  Measured on
// 2026-08-23 in the browser: loading a Tube spec ("Rings", index 20 of 904) and
// two DebrisCloud specs ("Geometry_*") each leave an EARLIER spec's name string
// released or dangling -- 3 of 904 after a full load.  Every read of those names
// is undefined behaviour; Win32 silently walked whatever the freed block held,
// but in WebAssembly a dangling pointer above the heap traps, and a trap inside
// the main-loop callback stops the game with no further output.
//
// Find() is the only code that touches all 904 names, so it is where the browser
// has to be defensive.  This is a wasm-only readability test; native keeps the
// original behaviour exactly.  The underlying loader defect is recorded for
// separate work -- do NOT treat this guard as that fix.
static bool MC2_NameIsReadable(const char* p)
{
	if (!p) return false;
	const uintptr_t limit =
		static_cast<uintptr_t>(__builtin_wasm_memory_size(0)) * 65536u;
	return static_cast<uintptr_t>(reinterpret_cast<uintptr_t>(p)) < limit;
}
#endif

//==========================================================================//
// File:	 gosFX_Effect.cpp												//
// Contents: Base gosFX::Effect Component									//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

gosFX::EffectLibrary*
	gosFX::EffectLibrary::Instance = NULL;

//------------------------------------------------------------------------------
//
void
	gosFX::EffectLibrary::InitializeClass()
{
	Verify(!Instance);
}

//------------------------------------------------------------------------------
//
void
	gosFX::EffectLibrary::TerminateClass()
{
	if (Instance)
	{
		Unregister_Object(Instance);
		delete Instance;
		Instance=NULL;
	}
}

//------------------------------------------------------------------------------
//
gosFX::EffectLibrary::EffectLibrary()
{
	Verify(gos_GetCurrentHeap() == Heap);
}

//------------------------------------------------------------------------------
//
gosFX::EffectLibrary::~EffectLibrary()
{
	for (unsigned i=0; i<m_effects.GetLength(); ++i)
	{
		if (m_effects[i])
		{
			Unregister_Object(m_effects[i]);
			delete m_effects[i];
		}
	}
}

//------------------------------------------------------------------------------
//
void
	gosFX::EffectLibrary::Load(Stuff::MemoryStream* stream)
{
	Verify(gos_GetCurrentHeap() == Heap);
	Verify(!m_effects.GetLength());
	int version = ReadGFXVersion(stream);
	unsigned len;
	*stream >> len;
	m_effects.SetLength(len);
	for (unsigned i=0; i<len; ++i)
	{
		m_effects[i] = gosFX::Effect::Specification::Create(stream, version);
		Check_Object(m_effects[i]);
		m_effects[i]->m_effectID = i;
	}
}

#if defined(__EMSCRIPTEN__)
// PORT: "no library, no effects" -- an invariant, enforced.
//
// With mc2.fx deferred the library is EMPTY, MakeEffect is never called, and
// every gosFX::Effect* an object holds must be NULL.  A non-NULL pointer is
// therefore stale memory rather than an effect, and calling through it traps.
// Captured from a live owner session on 2026-08-24, on the shipped artifact:
//
//   RuntimeError: memory access out of bounds
//     gosFX::Effect::Execute(...)  <- WeaponBolt::update()  <- browserMainLoop()
//
// Win32 walked whatever the stale block held; in WebAssembly the trap escapes
// the main-loop callback and the game stops dead with no further output. Drop
// the pointer instead of running it, and report once. This does NOT explain how
// the member goes stale -- that is a separate, still-open memory defect and must
// not be treated as closed because this guard exists.
void gosFX::DropEffectWithoutLibrary(const char* which, gosFX::Effect*& effect)
{
	if (!effect)
		return;
	if (EffectLibrary::Instance && EffectLibrary::Instance->m_effects.GetLength() > 0)
		return;                  // a real library is loaded; this is a real effect
	static bool reported = false;
	if (!reported)
	{
		reported = true;
		std::printf("[gosFX] dropped a stale '%s' effect (%p): the effect library is\n"
		            "[gosFX] empty, so no effect can legitimately exist here.\n",
		            which,(void*)effect);
	}
	effect = NULL;
}
#endif

#if defined(__EMSCRIPTEN__)
// PORT: the single answer to "should the browser load mc2.fx?".
//
// There are TWO loaders -- MechCmd2.cpp at startup and MC_TextureManager::flush()
// on screen transitions -- and only the first one was deferred. That is why the
// direct ?mission= route was effect-free and clean while the retail menu ->
// campaign route the owner actually plays reloaded all 904 specs behind the
// deferral and then trapped in gosFX::Effect::Execute during combat. Measured
// 2026-08-24: "[gosFX] live 'gosEffect' effect seen with a library of 904
// spec(s)" on the campaign route, with startup still printing "deferred".
//
// M8-B75, 2026-09-25: ON by default. The "mc2.fx is overwritten in memory" defect
// was Stuff::MemoryBlock laying its record pools over their own gos_Heap nodes
// (new(heap) T[n] bound to placement new[]; fixed in GameOS.HPP/gos_memory.cpp).
// MC2_BROWSER_EFFECTS=0 still switches the library off.
bool gosFX::BrowserEffectsEnabled()
{
	static int enabled = -1;
	if (enabled < 0)
	{
		const char* value = getenv("MC2_BROWSER_EFFECTS");
		enabled = (value && strcmp(value,"0") == 0) ? 0 : 1;
	}
	return enabled != 0;
}
#endif

//------------------------------------------------------------------------------
//
void
	gosFX::EffectLibrary::Save(Stuff::MemoryStream* stream)
{
	WriteGFXVersion(stream);
	*stream << m_effects.GetLength();
	for (unsigned i=0; i<m_effects.GetLength(); ++i)
	{
		Check_Object(m_effects[i]);
		m_effects[i]->Save(stream);
	}
}

//------------------------------------------------------------------------------
//
gosFX::Effect::Specification*
	gosFX::EffectLibrary::Find(const char* name)
{
#if defined(__EMSCRIPTEN__)
	if (!name) return NULL;
	unsigned unreadableNames = 0;
#endif
	for (unsigned i=0; i<m_effects.GetLength(); ++i)
	{
		gosFX::Effect::Specification *spec = m_effects[i];
		if (spec)
		{
			Check_Object(spec);
#if defined(__EMSCRIPTEN__)
			if (!MC2_NameIsReadable((char*)spec->m_name)) { ++unreadableNames; continue; }
#endif
			if (!_stricmp(spec->m_name, name))
			{
				Verify( spec->m_effectID == i );
				return spec;
			}
		}
	}
#if defined(__EMSCRIPTEN__)
	static bool reported = false;
	if (unreadableNames && !reported)
	{
		reported = true;
		std::printf("[gosFX] %u of %u effect name(s) in mc2.fx are unreadable and were\n"
		            "[gosFX] skipped; those effects cannot be looked up by name.\n",
		            unreadableNames,(unsigned)m_effects.GetLength());
	}
#endif
	return NULL;
}

//------------------------------------------------------------------------------
//
gosFX::Effect*
	gosFX::EffectLibrary::MakeEffect(
		unsigned index,
		unsigned flags
	)
{
	// PORT: fail the effect, not the frame.  A caller that reads ->m_effectID off a
	// NULL Find() result passes a garbage index here.  On Win32 the out-of-range
	// m_effects[index] quietly read wild memory; in WebAssembly it traps with
	// "memory access out of bounds", and a trap inside the browser main-loop
	// callback stops the game outright.  Every caller already tolerates a NULL
	// effect and guards its later use.
	if (index >= m_effects.GetLength())
		return NULL;
	gosFX::Effect::Specification *spec = m_effects[index];
	if (!spec)
		return NULL;
	Check_Object(spec);
	gosFX::Effect::ClassData *data =
		Cast_Pointer(
			gosFX::Effect::ClassData*, 
			Stuff::RegisteredClass::FindClassData(spec->GetClassID())
		);
	Check_Object(data);
	Check_Pointer(data->effectFactory);
#if defined(__EMSCRIPTEN__)
	++g_mc2EffectsMade;
#endif
	return (*data->effectFactory)(spec, flags);
}

#if defined(__EMSCRIPTEN__)
// PORT: M8-B75. How many effects the library has built this session, so a
// harness (Module._mc2_fx_made()) can tell "effects loaded" from "effects made".
extern "C" EMSCRIPTEN_KEEPALIVE unsigned mc2_fx_made(void)
{
	return g_mc2EffectsMade;
}
#endif
