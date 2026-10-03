//-----------------------------------------------------------------------------
// Wrapper to simplify using the ARM API from C++, via managed -> COM interop.
//
// Microsoft XNA
// Copyright (C) Microsoft Corporation. All rights reserved.
//-----------------------------------------------------------------------------
//
// PORT NOTE
// ---------
// The original header did:
//
//     #include <comdef.h>
//     #import "Microsoft.Xna.Arm.tlb" no_namespace
//
// `#import` of a COM type library is an MSVC-only extension that clang-cl does
// not implement, so this header could not be compiled as written.
//
// ARM is Microsoft's XNA-era Asset Resource Management service -- it records
// asset provenance and relationships while CONTENT IS BUILT. Its only use in
// this codebase is TG_TypeMultiShape::LoadTGMultiShapeFromASE (MCLib/msl.cpp),
// which parses 3D Studio MAX .ASE source art. The shipping game never walks
// that path: it loads pre-built binary shapes, and msl.h already declares the
// parameter as `IProviderEngine* armProvider = NULL`. Every call is guarded by
// `if (armProvider)` / `if (armAssetPtr)`.
//
// So the COM plumbing is replaced with a minimal set of declarations that
// preserve the exact signatures msl.cpp expects. Raw pointers satisfy every use
// there (assignment from NULL, truth-testing, comparison against NULL, and
// `->`), so the guarded blocks simply never execute -- identical behaviour to a
// build where no ARM provider was supplied, which is what the game always did.
//
// If asset tracking is ever wanted again, restore the original by defining
// MC2_USE_XNA_ARM and building with a toolchain that supports #import.
//-----------------------------------------------------------------------------

#ifndef __MICROSOFT_XNA_ARM_H__
#define __MICROSOFT_XNA_ARM_H__

#if defined(MC2_USE_XNA_ARM)

#include <comdef.h>

namespace Microsoft
{
	namespace Xna
	{
		namespace Arm
		{
			#import "Microsoft.Xna.Arm.tlb" no_namespace

			inline IProviderEnginePtr CreateProviderEngine(const char *toolName, const char *toolVersion)
			{
				IComFactory *factory = NULL;

				HRESULT hr = CoCreateInstance(__uuidof(ComFactory),
										  NULL,
										  CLSCTX_INPROC_SERVER,
										  __uuidof(IComFactory),
										  reinterpret_cast<void **>(&factory));

				if (FAILED(hr))
					return NULL;

				IProviderEnginePtr provider = factory->Create(toolName, toolVersion);

				factory->Release();

				return provider;
			}
		}
	}
}

#else	// ---- PORT: non-COM stand-in, see the note above ----

namespace Microsoft
{
	namespace Xna
	{
		namespace Arm
		{
			enum AssetType
			{
				AssetType_Physical = 0,
				AssetType_Logical  = 1
			};

			enum ProviderType
			{
				ProviderType_Primary   = 0,
				ProviderType_Secondary = 1
			};

			struct IProviderAsset
			{
				virtual void AddProperty(const char *name, const char *value) = 0;
				virtual void AddRelationship(const char *name, const char *value) = 0;
				virtual void Close() = 0;

			protected:
				~IProviderAsset() {}
			};

			typedef IProviderAsset* IProviderAssetPtr;

			struct IProviderEngine
			{
				virtual IProviderAssetPtr OpenAsset(
					const char *name,
					AssetType assetType,
					ProviderType providerType) = 0;

			protected:
				~IProviderEngine() {}
			};

			typedef IProviderEngine* IProviderEnginePtr;

			// No ARM service is available in this build; callers treat NULL as
			// "asset tracking disabled", which is the shipping configuration.
			inline IProviderEnginePtr CreateProviderEngine(const char * /*toolName*/,
														   const char * /*toolVersion*/)
			{
				return 0;
			}
		}
	}
}

#endif	// MC2_USE_XNA_ARM

#endif
