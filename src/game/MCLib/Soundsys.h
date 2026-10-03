//---------------------------------------------------------------------------
// soundsys.h - This file is the sound system header
//
// MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef SOUNDSYS_H
#define SOUNDSYS_H
//---------------------------------------------------------------------------
// Include files
#ifndef DSTD_H
#include "dstd.h"
#endif

#ifndef HEAP_H
#include "Heap.h" // PORT: match case-sensitive host path.
#endif

#ifndef PACKET_H
#include "packet.h"
#endif

#ifndef CIDENT_H
#include "Cident.h" // PORT: match case-sensitive host path.
#endif

#ifndef PATHS_H
#include "Paths.h" // PORT: match case-sensitive host path.
#endif

#ifndef INIFILE_H
#include "Inifile.h" // PORT: match case-sensitive host path.
#endif

#ifndef TIMING_H
#include "Timing.h" // PORT: match case-sensitive host path.
#endif

#include <math.h>
#include <GameOS.HPP> // PORT: match case-sensitive host path.
#include <Stuff/Stuff.hpp> // PORT: use host-independent include separators.
//---------------------------------------------------------------------------
// Macro Defintiions
// Channels 0 through 10 are Main Sound Effect Channels
// Channels 11 through 12 are ambient sound channels
// Channel 13 is Support Officer
// Channel 14 is Betty
// Channel 15 is Fire Burning SFX
#define MAX_DIGITAL_SAMPLES			16

#define MAX_SAMPLE_VOLUME			127

#define RADIO_CHANNEL				0
#define FIRE_CHANNEL				(MAX_DIGITAL_SAMPLES-1)
#define BETTY_CHANNEL				(FIRE_CHANNEL-1)
#define SUPPORT_CHANNEL				(BETTY_CHANNEL-1)

#define AMBIENT_CHANNEL_START		11

#define STREAM1CHANNEL				16
#define STREAM2CHANNEL				17
#define STREAM3CHANNEL				18		//Used for in-Mission voiceovers

#define MAX_QUEUED_MESSAGES			8
//---------------------------------------------------------------------------
// Classes
#pragma pack(1)
typedef struct {     
	WORD  wFormatTag;     
	WORD  nChannels; 
    DWORD nSamplesPerSec;     
	DWORD nAvgBytesPerSec;     
	WORD  nBlockAlign; 
} MC2_WAVEFORMATEX;
#pragma pack()

//---------------------------------------------------------------------------
typedef struct _SoundBite
{
	unsigned long		priority;
	unsigned long		cacheStatus;
	unsigned long		soundId;
	unsigned long 		biteSize;
	MemoryPtr			biteData;
	float				volume;
	HGOSAUDIO			resourceHandle;
} SoundBite;

//---------------------------------------------------------------------------
class SoundSystem
{
	//Data Members
	//-------------
	protected:
		bool				channelInUse[MAX_DIGITAL_SAMPLES];
		long				channelSampleId[MAX_DIGITAL_SAMPLES];
		bool				fadeDown[MAX_DIGITAL_SAMPLES];
		
		unsigned long		soundHeapSize;
		UserHeapPtr			soundHeap;
		
		unsigned long		numSoundBites;
		SoundBite			*sounds;

		unsigned long		numBettySamples;
		unsigned long		numSupportSamples;
		PacketFilePtr		soundDataFile;
		PacketFilePtr		bettyDataFile;
		PacketFilePtr		supportDataFile;
		
		long				numDMS;
		float				streamFadeDownTime;
		unsigned long		digitalStreamBitDepth;
		unsigned long		digitalStreamChannels;
		long				currentMusicId;
		
		HGOSAUDIO			stream1Handle;
		bool				stream1Active;
		float				stream1Time;
		long				stream1Id;
		
		HGOSAUDIO			stream2Handle;
		bool				stream2Active;
		float				stream2Time;
		long				stream2Id;
		
		HGOSAUDIO			stream3Handle;
		bool				stream3Active;

		char				**digitalMusicIds;
		bool				*digitalMusicLoopFlags;
		float				*digitalMusicVolume;
				
		float				maxSoundDistance;
		
		MemoryPtr			bettySoundBite;							//Used to store current bitching betty sample
		long				lastBettyId;
		HGOSAUDIO			bettyHandle;

		MemoryPtr			supportSoundBite;						//USed to store current support officer sample
		long				lastSupportId;
		HGOSAUDIO			supportHandle;
		
		DWORD				isRaining;								//What sound FX should play as an ambient background loop.  Rain/Fire/Nothing, etc.
		DWORD				oldRaining;								//Check if the old rain doesn't match new.  If so, play new instead!

		float				sensorBeepUpdateTime;
		
		bool				gamePaused;								//Flag to tell me if game is paused

		static float		digitalMasterVolume;
		static float		SFXVolume;
		static float		musicVolume;
		static float		radioVolume;
		static float		bettyVolume;

	public:

		static long			largestSensorContact;
		
	//Member Functions
	//----------------
	public:
	
		SoundSystem (void)
		{
			init();
		}
		
		~SoundSystem (void)
		{
			destroy();
		}
		
		void init (void)
		{
			memset(channelInUse,0,sizeof(bool)*MAX_DIGITAL_SAMPLES);
			memset(channelSampleId,-1,sizeof(long)*MAX_DIGITAL_SAMPLES);
			memset(fadeDown,0,sizeof(bool)*MAX_DIGITAL_SAMPLES);
			
			soundHeapSize = 0;
			soundHeap = NULL;

			numSoundBites = 0;
			sounds = NULL;
			
			soundDataFile = bettyDataFile = NULL;
			
			numDMS = 0;
			streamFadeDownTime = 0.0;
			digitalStreamBitDepth = 0;
			digitalStreamChannels = 0;
			
			digitalMusicIds = NULL;
			digitalMusicLoopFlags = NULL;
			
			maxSoundDistance = 0.0;
			
			bettySoundBite = NULL;
			lastBettyId = -1;
			
			currentMusicId = -1;
			
			stream1Handle = 0;
			stream1Active = FALSE;
			stream1Time = 0.0;
			
			stream2Handle = 0;
			stream2Active = FALSE;
			stream2Time = 0.0;
			
			stream3Handle = 0;
			stream3Active = false;
			
			numBettySamples = 0;
			bettyHandle = NULL;

			supportSoundBite = NULL;
			lastSupportId = -1;
			
			numSupportSamples = 0;
			supportHandle = NULL;
			
			isRaining = oldRaining = 0;
			sensorBeepUpdateTime = 0.0f;
			gamePaused = false;
		}
		
		long init (char *soundFileName);
		virtual void destroy (void);
		
		void preloadSoundBite (long sampleId);
		long findOpenChannel (long start, long end);

		long playDigitalSample (unsigned long sampleId, Stuff::Vector3D pos = Stuff::Vector3D(-9999.0f,-9999.0,-9999.0f), bool allowDupes = false);
		
		long playDigitalMusic (long musicId);
		
		// PORT (M8-B35): BOTH of these now return the STREAM3 token they took,
		// or 0 if they took nothing. Pass that value back to stopSupportSample
		// so a caller can only ever silence its OWN sound. A caller that got 0
		// must not call stopSupportSample at all -- token 0 there means "stop
		// unconditionally", which is what the mission teardown paths want and
		// the exact opposite of what a caller with no claim wants.
		long playDigitalStream (const char *streamName);

		// PORT (M8-1): the same STREAM3 channel and the same lifetime as
		// playDigitalStream, fed from memory instead of a .wav on disk. This is
		// how a movie's own soundtrack is heard: the owner ruled on 2026-08-15
		// that movie sound lives inside the movie file, and the decoder hands
		// back PCM, not a path. The CALLER OWNS `pcm` and must keep it alive
		// until stopSupportSample() -- gosAudio_UserMemory stores the pointer.
		// volumeScale multiplies this channel's volume above the normal
		// digitalMasterVolume * bettyVolume, clamped at the 1.0 the slider
		// allows. M8-B5 needs it because a movie's own samples can only be
		// scaled up to full scale, and the owner's requested boost is larger
		// than the headroom some tracks have.
		long playDigitalMemory (void *pcm, long bytes, long channels, long samplesPerSecond,
								float volumeScale = 1.0f);

		bool isDigitalMusicPlaying (void);
		bool isPlayingSample(long sampleId);
		bool isChannelPlaying (long channelId);
		bool isPlayingVoiceOver (void);

		long playBettySample (unsigned long bettySampleId);
		void stopBettySample (void);

		
		long playSupportSample (unsigned long supportSampleId, char* fileName = NULL);

		// Bumped every time something takes STREAM3. Never reused.
		unsigned long	stream3Token;
		
 		void stopDigitalSample (unsigned long sampleHandleNumber);
		void stopDigitalMusic (void);
		// PORT (M8-B28): `why` is a trace tag only -- it names the call site in
		// the MC2_AUDIO_CENSUS log so a STREAM3 that goes quiet mid-movie can be
		// attributed. It changes no behaviour.
		// PORT (M8-B28): STREAM3 NOW HAS AN OWNER, because it never did.
		//
		// MEASURED, playtest-20260829-140911.err.log, the owner's Eclipse run:
		//   line 5656  'w09' audio started on STREAM3
		//   line 5685  STREAM3 STOPPED by MC2Movie      <-- 60 lines EARLIER
		//   line 5745  'w09' stopping sound; mode=4 (already finished)
		// Something else stopped the channel while w09 was still talking, and
		// w09 then found it already dead. That is the owner's voice line cut
		// "shortly after saying skipper", with a timestamp on it.
		//
		// stopMovieSound fires on (separateWAVE || m_audioStarted), so a movie
		// that uses a separate WAVE file and NEVER TOUCHED STREAM3 still stops
		// it on teardown -- which is why no second "audio started" line appears
		// between those two events.
		//
		// `token` is the value getStream3Token() returned when the caller's own
		// audio started. A non-zero token that no longer matches means the
		// caller has been superseded and its stop is IGNORED. Token 0 keeps the
		// old unconditional behaviour, which is what MissionBegin,
		// MissionResults and the ABL script teardown actually want.
		unsigned long getStream3Token (void) { return stream3Token; }
		void stopSupportSample (const char* why = "unspecified", unsigned long token = 0);
	
		void setDigitalMasterVolume (byte volume);
		long getDigitalMasterVolume (void);
		
		unsigned char getSFXVolume();
		unsigned char getRadioVolume();
		unsigned char getMusicVolume();
		unsigned char getBettyVolume();

		void setSFXVolume(unsigned char volume);
		void setRadioVolume(unsigned char volume);
		void setMusicVolume(unsigned char volume);
		void setBettyVolume(unsigned char volume);

		virtual void update (void);
		
		void playABLDigitalMusic (long musicId);
		void stopABLMusic (void);
		long getCurrentMusicId (void)
		{
			return currentMusicId;
		}
		
		void playABLSFX (long sfxId);
		void playABLVideo (long videoId);
		void playABLSpeech (long pilotId, long speechId);
		
		long playPilotSpeech (char *pilotSpeechFilename, long msgId);
		
		void setIsRaining (DWORD rainLevel)
		{
			isRaining = rainLevel;
		}
		
		void setIsPaused (void)
		{
			gamePaused = true;
		}
		
		void clearIsPaused (void)
		{
			gamePaused = false;
		}
};

//---------------------------------------------------------------------------

extern bool useSound;
extern bool useMusic;

//---------------------------------------------------------------------------
#endif
