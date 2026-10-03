//---------------------------------------------------------------------------
// soundsys.cpp - This file is the sound system code
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include files
#ifndef SOUNDSYS_H
#include "Soundsys.h" // PORT: match case-sensitive host path.
#endif

#ifndef SOUNDS_H
#include "Sounds.h" // PORT: match case-sensitive host path.
#endif

#ifndef CAMERA_H
#include "Camera.h" // PORT: match case-sensitive host path.
#endif

//---------------------------------------------------------------------------
// static globals
bool useSound = TRUE;
bool useMusic = TRUE;

long DigitalMasterVolume = 0;
long MusicVolume = 0;
long sfxVolume = 0;
long RadioVolume = 0;
long BettyVolume = 0;

#define MAX_SENSOR_UPDATE_TIME		(5.0f)	//Seconds

float		SoundSystem::digitalMasterVolume = 0.99f;
float		SoundSystem::SFXVolume = 0.6f;
float		SoundSystem::musicVolume = 0.4f;
float		SoundSystem::radioVolume = 0.6f;
float		SoundSystem::bettyVolume = 0.7f;
long		SoundSystem::largestSensorContact = -1;

#define FALLOFF_DISTANCE			(1500.0f)
//---------------------------------------------------------------------------
// class SoundSystem 
//---------------------------------------------------------------------------
// PORT (M8-B28): STREAM3 IS SHARED AND NOTHING SAID WHO HAD IT.
//
// Owner report 2026-08-26, prison rescue mission: a Renard/Baxter video played
// in the corner, its voice started, then "one of my units making a call out
// overrode the story dialogue" and "the video continued on without sound".
//
// Pilot call-outs are radio messages on RADIO_CHANNEL (0), not STREAM3 (18), so
// a call-out cannot take this channel directly -- which is exactly why guessing
// is no good here. What CAN silence a playing movie is stopSupportSample(),
// which destroys the resource unconditionally and is called from six places, or
// playDigitalStream() replacing the resource. This names every transition so the
// next playtest log answers it instead of a theory.
//
// Gated on MC2_AUDIO_CENSUS because the strict bar hashes stderr.
static void stream3Trace (const char* what, const char* detail)
{
	const char* c = getenv("MC2_AUDIO_CENSUS");
	if (!c || !*c || strcmp(c, "0") == 0)
		return;
	fprintf(stderr, "[soundsys] STREAM3 %s%s%s\n", what,
	        (detail && *detail) ? " " : "", (detail && *detail) ? detail : "");
	fflush(stderr);
}

void SoundSystem::destroy (void)
{
	if (soundHeap)
	{
		soundHeap->destroy();

		delete soundHeap;
		soundHeap = NULL;
	}
}

//---------------------------------------------------------------------------
long SoundSystem::init (char *soundFileName)
{
	long i;
	if (useSound)
	{
		FullPathFileName soundName;
		soundName.init(soundPath,soundFileName,".snd");
		
		FitIniFile soundFile;
		long result = soundFile.open(soundName);
#if defined(__EMSCRIPTEN__)
		// PORT (C2 bring-up): every gosASSERT below compiles out in release, so a
		// failed open here continues silently and the game simply never plays
		// anything. Browser-only so the native strict bar cannot move.
		std::printf("[soundinit] sound.snd open=%ld\n", result);
#endif
		gosASSERT(result == NO_ERR);

		result = soundFile.seekBlock("SoundSetup");
		gosASSERT(result == NO_ERR);

		result = soundFile.readIdULong("soundHeapSize",soundHeapSize);
		gosASSERT(result == NO_ERR);
		
		result = soundFile.readIdFloat("MaxSoundDistance",maxSoundDistance);
		gosASSERT(result == NO_ERR);

		soundHeap = new UserHeap;
		gosASSERT(soundHeap != NULL);

		result = soundHeap->init(soundHeapSize,"SOUND");
		gosASSERT(result == NO_ERR);

		//-----------------------------------------------------------------------
		// Startup the Sound packet File with the sound Blocks in it.
		// This works by sound ID.  The sound ID is the packet number.
		// When ordered to play a sample, the sound system check to see if that
		// sound ID is in the cache.  If not, it is loaded.  If there is no more
		// room, any idle sounds are flushed in order of priority.
		soundDataFile = new PacketFile;
		gosASSERT(soundDataFile != NULL);
		
		FullPathFileName soundDataPath;
		soundDataPath.init(CDsoundPath,soundFileName,".pak");
		
		result = soundDataFile->open(soundDataPath);
#if defined(__EMSCRIPTEN__)
		std::printf("[soundinit] sound.pak open=%ld packets=%ld\n", result, result == NO_ERR ? soundDataFile->getNumPackets() : -1L);
#endif
		gosASSERT(result == NO_ERR);
		
		bettyDataFile = new PacketFile;
		gosASSERT(bettyDataFile != NULL);
		
		FullPathFileName bettyDataPath;
		bettyDataPath.init(CDsoundPath,"Betty",".pak");
		
		result = bettyDataFile->open(bettyDataPath);
#if defined(__EMSCRIPTEN__)
		std::printf("[soundinit] betty.pak open=%ld\n", result);
#endif
		gosASSERT(result == NO_ERR);

		numBettySamples = bettyDataFile->getNumPackets();
		
		supportDataFile = new PacketFile;
		gosASSERT(supportDataFile != NULL);
		
		FullPathFileName supportDataPath;
		supportDataPath.init(CDsoundPath,"support",".pak");
		
		result = supportDataFile->open(supportDataPath);
#if defined(__EMSCRIPTEN__)
		std::printf("[soundinit] support.pak open=%ld\n", result);
#endif
		gosASSERT(result == NO_ERR);

		numSupportSamples = supportDataFile->getNumPackets();
 		//-----------------------------------------------------------------------
		// Load all of the sound Bite data.  Do not load actual packet unless
		// preload field is TRUE.
		result = soundFile.seekBlock("SoundBites");
		gosASSERT(result == NO_ERR);

		result = soundFile.readIdULong("numBites",numSoundBites);
		gosASSERT(result == NO_ERR);
#if defined(__EMSCRIPTEN__)
		std::printf("[soundinit] numBites=%lu (read=%ld)\n", (unsigned long)numSoundBites, result);
#endif

		//-----------------------------------------------------------------------
		// Preallocate SoundBites
		sounds = (SoundBite *)soundHeap->Malloc(sizeof(SoundBite) * numSoundBites);
		gosASSERT(sounds != NULL);
		memset(sounds,0,sizeof(SoundBite) * numSoundBites);
		
		for (i=0;i<(long)numSoundBites;i++)
		{
			char biteBlock[20];
			sprintf(biteBlock,"SoundBite%d",i);
			
			result = soundFile.seekBlock(biteBlock);
			gosASSERT(result == NO_ERR);
			
			result = soundFile.readIdULong("priority",sounds[i].priority);
			gosASSERT(result == NO_ERR);
			
			result = soundFile.readIdULong("cache",sounds[i].cacheStatus);
			gosASSERT(result == NO_ERR);
			
			result = soundFile.readIdULong("soundId",sounds[i].soundId);
			gosASSERT(result == NO_ERR);
			
			preloadSoundBite(i);		//ALWAYS Preload!!!!!!!!!!!!!

			result = soundFile.readIdFloat("volume",sounds[i].volume);
			gosASSERT(result == NO_ERR);
		}		

		//---------------------------------------------------------------
		// Load the digital Music Data Strings
		result = soundFile.seekBlock("DigitalMusic");
		gosASSERT(result == NO_ERR);
			
		result = soundFile.readIdLong("NumDMS",numDMS);
		gosASSERT(result == NO_ERR);
#if defined(__EMSCRIPTEN__)
		std::printf("[soundinit] numDMS=%ld (read=%ld)\n", (long)numDMS, result);
#endif

		result = soundFile.readIdFloat("StreamFadeDownTime",streamFadeDownTime);
		gosASSERT(result == NO_ERR);

		result = soundFile.readIdULong("StreamBitDepth",digitalStreamBitDepth);
		gosASSERT(result == NO_ERR);
			
		result = soundFile.readIdULong("StreamChannels",digitalStreamChannels);
		gosASSERT(result == NO_ERR);

		digitalMusicIds = (char **)soundHeap->Malloc(sizeof(char *) * numDMS);
		gosASSERT(digitalMusicIds != NULL);
			
		digitalMusicLoopFlags = (bool *)soundHeap->Malloc(sizeof(bool)*numDMS);
		gosASSERT(digitalMusicLoopFlags != NULL);

		digitalMusicVolume = (float *)soundHeap->Malloc(sizeof(float) * numDMS);
			
		for (i=0;i<numDMS;i++)
		{
			char digitalMSId[20];
			sprintf(digitalMSId,"DMS%d",i);
			
			char digitalMSBId[20];
			sprintf(digitalMSBId,"DMSLoop%d",i);

			char digitalMSVId[25];
			sprintf(digitalMSVId,"DMSVolume%d",i);
			
			digitalMusicIds[i] = (char *)soundHeap->Malloc(30);
			result = soundFile.readIdString(digitalMSId,digitalMusicIds[i],29);
			gosASSERT(result == NO_ERR);
			
			result = soundFile.readIdBoolean(digitalMSBId,digitalMusicLoopFlags[i]);
			gosASSERT(result == NO_ERR);

			result = soundFile.readIdFloat(digitalMSVId,digitalMusicVolume[i]);
			if (result != NO_ERR)
				digitalMusicVolume[i] = 1.0f;
		}	
		
		soundFile.close();
		
		for (i=0;i<MAX_DIGITAL_SAMPLES+4;i++)
		{
			gosAudio_AllocateChannelSliders(i,gosAudio_Volume | gosAudio_Panning);
		}
	}
	
	digitalMasterVolume = float(DigitalMasterVolume) / 256.0f;
	SFXVolume = float(sfxVolume) / 256.0f;
	musicVolume = float(MusicVolume) / 256.0f;
	radioVolume = float(RadioVolume) / 256.0f;
	bettyVolume = float(BettyVolume) / 256.0f;

	stream1Id = stream2Id = 0;

	return(NO_ERR);
}

//////////////////////////////////////////////////////////////////
//
// wave_ParseWaveMemory
//   Parses a chunk of memory into the header and samples.
//   This is done by looking for the "fmt " and "data"
//   fields in the memory.
//
//////////////////////////////////////////////////////////////////
// PORT: wave_ParseWaveMemory (defined below) returns FALSE and leaves the header
// NULL on five paths, and none of its call sites check. Every one dereferences
// it on the next line. Same defect and same fix as gamesound.cpp -- see the
// comment there for the crash this came from. An empty-but-valid format lets the
// fragment be skipped instead of faulting; nChannels is never zero because
// callers divide by it.
static MC2_WAVEFORMATEX* wave_EmptyFormat (void)
{
	static MC2_WAVEFORMATEX empty;
	empty.wFormatTag      = 1;       // PCM
	empty.nChannels       = 1;
	empty.nSamplesPerSec  = 22050;
	empty.nAvgBytesPerSec = 22050;
	empty.nBlockAlign     = 1;
	return &empty;
}

bool wave_ParseWaveMemory(MemoryPtr lpChunkOfMemory, MC2_WAVEFORMATEX** lplpWaveHeader, MemoryPtr* lplpWaveSamples,DWORD *lpcbWaveSize)
{
    DWORD 	*pdw;
    DWORD 	*pdwEnd;
    DWORD   dwRiff;
    DWORD   dwType;
    DWORD   dwLength;

    // Set defaults to NULL or zero
    if (*lplpWaveHeader)
        *lplpWaveHeader = NULL;

    if (*lplpWaveSamples)
        *lplpWaveSamples = NULL;

    if (lpcbWaveSize)
        *lpcbWaveSize = 0;

    // Set up DWORD pointers to the start of the chunk
    // of memory.
    pdw = (DWORD *)lpChunkOfMemory;

    // Get the type and length of the chunk of memory
    dwRiff = *pdw++;
    dwLength = *pdw++;
    dwType = *pdw++;

    // Using the mmioFOURCC macro (part of Windows SDK), ensure
    // that this is a RIFF WAVE chunk of memory
    if (dwRiff != 0x46464952)
      return FALSE;      // not even RIFF

    if (dwType != 0x45564157)
      return FALSE;      // not a WAV

    // Find the pointer to the end of the chunk of memory
    pdwEnd = (DWORD *)((BYTE *)pdw + dwLength-4);

    // Run through the bytes looking for the tags
    while (pdw < pdwEnd)
      {
      dwType   = *pdw++;
      dwLength = *pdw++;

      switch (dwType)
        {
        // Found the format part
        case 0x20746d66:

          if (*lplpWaveHeader == NULL)
            {
            if (dwLength < sizeof(MC2_WAVEFORMATEX))
              return FALSE; // something's wrong! Not a WAV

            // Set the lplpWaveHeader to point to this part of
            // the memory chunk
            *lplpWaveHeader = (MC2_WAVEFORMATEX*)pdw;

            // Check to see if the other two items have been
            // filled out yet (the bits and the size of the
            // bits). If so, then this chunk of memory has
            // been parsed out and we can exit
            if ((!lplpWaveSamples || *lplpWaveSamples) &&
                (!lpcbWaveSize || *lpcbWaveSize))
              {
              return TRUE;
              }
            }
          break;

        // Found the samples
        case 0x61746164:

          if ((lplpWaveSamples && !*lplpWaveSamples) ||
              (lpcbWaveSize && !*lpcbWaveSize))
            {
            // Point the samples pointer to this part of the
            // chunk of memory.
            if (*lplpWaveSamples == NULL)
				*lplpWaveSamples = (MemoryPtr)pdw;

            // Set the size of the wave
            if (lpcbWaveSize)    *lpcbWaveSize = dwLength;

            // Make sure we have our header pointer set up.
            // If we do, we can exit
            if (lplpWaveHeader)
              return TRUE;
            }
          break;

        } // End case

      // Move the pointer through the chunk of memory
      pdw = (DWORD *)((BYTE *)pdw + ((dwLength+1)&~1));
      }

  // Failed! If we made it here, we did not get all the peices
  // of the wave
  return FALSE;
}

//---------------------------------------------------------------------------
void SoundSystem::preloadSoundBite (long soundId)
{
	long result = soundDataFile->seekPacket(soundId);
	if (result != NO_ERR)
		return;
		
	//-------------------------------------------------------------
	// If there is a packet in this spot, load it.
	// First, check if there is enough room in the sound heap
	// and if not, free up any samples whose cache status says dump.
	unsigned long packetSize = soundDataFile->getPacketSize();
	if (packetSize > 0)
	{
		SoundBite *thisSoundBite = (SoundBite *)(&(sounds[soundId]));
		thisSoundBite->resourceHandle = 0;
			
		if (thisSoundBite->biteSize == 0 || thisSoundBite->biteData == NULL)
		{
			thisSoundBite->biteSize = packetSize;
			thisSoundBite->biteData = (MemoryPtr)soundHeap->Malloc(packetSize);
			if (!thisSoundBite->biteData)
				return;
		}
		
		soundDataFile->readPacket(soundId,thisSoundBite->biteData);
		
		//--------------------------------------------------------------------
		// Hand GOS sound the data it needs to create the resource Handle
		gosAudio_Format soundFormat;
		soundFormat.wFormatTag = 1;				//PCM

		MC2_WAVEFORMATEX *waveFormat = NULL;
		MemoryPtr dataOffset = NULL;
		DWORD length = 0;
		DWORD bitsPerSec = 0;
		wave_ParseWaveMemory(thisSoundBite->biteData,&waveFormat,&dataOffset,&length);
		if (!waveFormat) waveFormat = wave_EmptyFormat();	// PORT: parse can fail and leave this NULL
	   
		if (waveFormat && dataOffset)
		{
			bitsPerSec = waveFormat->nBlockAlign / waveFormat->nChannels * 8;

			soundFormat.nChannels = waveFormat->nChannels;
			soundFormat.nSamplesPerSec = waveFormat->nSamplesPerSec;
			soundFormat.nAvgBytesPerSec = waveFormat->nAvgBytesPerSec;
			soundFormat.nBlockAlign = waveFormat->nBlockAlign;
			soundFormat.wBitsPerSample = bitsPerSec;
			soundFormat.cbSize = 0;

			gosAudio_CreateResource(&thisSoundBite->resourceHandle,gosAudio_UserMemory, NULL, &soundFormat,dataOffset,length);
		}
		else
		{
			thisSoundBite->resourceHandle = 0;
		}
	}
}

//---------------------------------------------------------------------------
void SoundSystem::update (void)
{
	long i;
#if defined(__EMSCRIPTEN__)
	// PORT (C2 bring-up): the music fade advances by frameLength per update and
	// sets the FULL volume only on the frame it crosses zero. A frameLength of 0
	// therefore parks the fade wherever it got to, and every later update writes
	// the SAME value -- which looks exactly like 'the volume was set once and
	// never again'. Report the driver and the four factors so the two separate.
	{
		static long mc2SndTick = 0;
		if ((mc2SndTick++ % 240) == 0)
			std::printf("[soundupd] frameLength=%.5f stream1Active=%d stream1Time=%.4f fadeTime=%.4f master=%.3f music=%.3f trackVol=%.3f id=%ld\n",
			            frameLength, stream1Active ? 1 : 0, stream1Time, streamFadeDownTime,
			            digitalMasterVolume, musicVolume,
			            (digitalMusicVolume && currentMusicId >= 0) ? digitalMusicVolume[currentMusicId] : -1.0f,
			            (long)currentMusicId);
	}
#endif
	if (useSound && (isRaining != oldRaining))
	{
		//Play the isRaining SoundEffect at the soundEffect level in a loop.
		if (isRaining && (isRaining < numSoundBites))
		{
			long ourChannel = FIRE_CHANNEL;
	
			gosAudio_SetChannelSlider(ourChannel,gosAudio_Panning, 0.0);
			float vol = sounds[isRaining].volume;
			gosAudio_SetChannelSlider(ourChannel,gosAudio_Volume, (digitalMasterVolume * vol * SFXVolume));
			channelSampleId[ourChannel] = isRaining;
			channelInUse[ourChannel] = TRUE;
				
			if (sounds[isRaining].biteData)
			{
				gosAudio_AssignResourceToChannel( ourChannel, sounds[isRaining].resourceHandle );
				gosAudio_SetChannelPlayMode( ourChannel, gosAudio_Loop );
			}
			
			oldRaining = isRaining;
		}
		else
		{
			//Stop the sound Effect by fading it to zero!
			oldRaining = isRaining;
			gosAudio_PlayMode sampleStatus = gosAudio_GetChannelPlayMode(FIRE_CHANNEL);
			if ( sampleStatus == gosAudio_PlayOnce || sampleStatus == gosAudio_Loop )
			{
				fadeDown[FIRE_CHANNEL] = TRUE;
			}
		}
 	}
	
	if (useSound && bettySoundBite)
	{
		//---------------------------------------------------
		// Check if betty is done.  If so, whack the memory
		gosAudio_PlayMode sampleStatus = gosAudio_GetChannelPlayMode(BETTY_CHANNEL);
		if (sampleStatus != gosAudio_PlayOnce)
		{
			gosAudio_DestroyResource(&bettyHandle);
			soundHeap->Free(bettySoundBite);
			bettySoundBite = NULL;
		}
	}
	
	if (useSound && supportSoundBite)
	{
		//---------------------------------------------------
		// Check if betty is done.  If so, whack the memory
		gosAudio_PlayMode sampleStatus = gosAudio_GetChannelPlayMode(SUPPORT_CHANNEL);
		if (sampleStatus != gosAudio_PlayOnce)
		{
			gosAudio_DestroyResource(&supportHandle);
			soundHeap->Free(supportSoundBite);
			supportSoundBite = NULL;
		}
	}
	
	if (useSound && stream3Handle)
	{
		gosAudio_PlayMode sampleStatus = gosAudio_GetChannelPlayMode(STREAM3CHANNEL);
		if (!stream3Active && (sampleStatus != gosAudio_PlayOnce))
		{
			stream3Active = true;
			gosAudio_SetChannelPlayMode( STREAM3CHANNEL, gosAudio_PlayOnce );
		}
				
		sampleStatus = gosAudio_GetChannelPlayMode(STREAM3CHANNEL);
		if (stream3Active && sampleStatus != gosAudio_PlayOnce)
		{
			gosAudio_DestroyResource(&stream3Handle);
			stream3Handle = 0;
	stream3Token = 0;
			stream3Active = false;
			stream3Trace("finished on its own", "");
		}
	}
	
	if ((largestSensorContact > -1) && (sensorBeepUpdateTime > MAX_SENSOR_UPDATE_TIME))
	{
		playDigitalSample(SENSOR2);
		sensorBeepUpdateTime = 0.0f;
	}

	//Start the music change we requested playing!!
	// This avoids the stupid sound bug of starting more then one stream per frame!!
	if (stream1Handle && !stream1Active)
	{
		stream1Active = true;
		gosAudio_SetChannelPlayMode( STREAM1CHANNEL, digitalMusicLoopFlags[currentMusicId] ? gosAudio_Loop : gosAudio_PlayOnce);
	}

	if (stream2Handle && !stream2Active)
	{
		stream2Active = true;
		gosAudio_SetChannelPlayMode( STREAM2CHANNEL, digitalMusicLoopFlags[currentMusicId] ? gosAudio_Loop : gosAudio_PlayOnce);
	}

 	if (useMusic && (stream1Active || stream2Active))
	{
		if (stream1Active && (stream1Time != 0.0))
		{
			if (stream1Time < 0.0)
			{
				//------------------------------------------
				// We are fading Down.
				stream1Time += frameLength;
				if (stream1Time >= 0.0)
				{
					stream1Time = 0.0;
					if (stream1Handle) 
						gosAudio_DestroyResource(&stream1Handle);
					stream1Handle = NULL;
					stream1Active = FALSE;
				}
				else
				{
					float volLevel = fabs(stream1Time) / streamFadeDownTime;
					if (volLevel < 0.0)
						volLevel = 0.0;

					if (volLevel > 1.0)
						volLevel = 1.0;

					gosAudio_SetChannelSlider(STREAM1CHANNEL, gosAudio_Volume, digitalMasterVolume * volLevel * musicVolume * digitalMusicVolume[currentMusicId]);
				}
			}
			else if (stream1Time > 0.0)
			{
				//------------------------------------------
				// We are fading Up.
				stream1Time -= frameLength;
				if (stream1Time <= 0.0)
				{
					stream1Time = 0.0;
					gosAudio_SetChannelSlider(STREAM1CHANNEL, gosAudio_Volume, digitalMasterVolume * musicVolume * digitalMusicVolume[currentMusicId]);
				}
				else
				{
					float volLevel = (streamFadeDownTime - fabs(stream1Time)) / streamFadeDownTime;
					
					if (volLevel < 0.0)
						volLevel = 0.0;

					if (volLevel > 1.0)
						volLevel = 1.0;

					gosAudio_SetChannelSlider(STREAM1CHANNEL, gosAudio_Volume, digitalMasterVolume * volLevel * musicVolume * digitalMusicVolume[currentMusicId]);
				}
			}
		}
			
		if (stream2Active && (stream2Time != 0.0))
		{
			if (stream2Time < 0.0)
			{
				//------------------------------------------
				// We are fading Down.
				stream2Time += frameLength;
				if (stream2Time >= 0.0)
				{
					stream2Time = 0.0;
					if (stream2Handle) 
						gosAudio_DestroyResource(&stream2Handle);
					stream2Handle = NULL;
					stream2Active = FALSE;
				}
				else
				{
					float volLevel = fabs(stream2Time) / streamFadeDownTime;

					if (volLevel < 0.0)
						volLevel = 0.0;

					if (volLevel > 1.0)
						volLevel = 1.0;

					gosAudio_SetChannelSlider(STREAM2CHANNEL, gosAudio_Volume, digitalMasterVolume * volLevel * musicVolume * digitalMusicVolume[currentMusicId]);
				}
			}
			else if (stream2Time > 0.0)
			{
				//------------------------------------------
				// We are fading Up.
				stream2Time -= frameLength;
				if (stream2Time <= 0.0)
				{
					stream2Time = 0.0;
					gosAudio_SetChannelSlider(STREAM2CHANNEL, gosAudio_Volume, digitalMasterVolume * musicVolume * digitalMusicVolume[currentMusicId]);
				}
				else
				{
					float volLevel = (streamFadeDownTime - fabs(stream2Time)) / streamFadeDownTime;

					if (volLevel < 0.0)
						volLevel = 0.0;

					if (volLevel > 1.0)
						volLevel = 1.0;

					gosAudio_SetChannelSlider(STREAM2CHANNEL, gosAudio_Volume, digitalMasterVolume * volLevel * musicVolume * digitalMusicVolume[currentMusicId]);
				}
			}
		}
			
		if (stream1Active && stream1Handle && (gosAudio_GetChannelPlayMode(STREAM1CHANNEL) == gosAudio_Stop))
		{
			if (stream1Handle) 
				gosAudio_DestroyResource(&stream1Handle);
				
			stream1Handle = NULL;
			stream1Time = 0.0;
			stream1Active = FALSE;
		}
		
		if (stream2Active && stream2Handle && (gosAudio_GetChannelPlayMode(STREAM2CHANNEL) == gosAudio_Stop))
		{
			if (stream2Handle) 
				gosAudio_DestroyResource(&stream2Handle);
				
			stream2Handle = NULL;
		 	stream2Time = 0.0;
		 	stream2Active = FALSE;
		}

		if (!stream1Active && !stream2Active)
			currentMusicId = -1;
	}

	if (useSound)
	{
		//-----------------------------------------------
		// Check all samples to see if one should end.
		for (i=0;i<MAX_DIGITAL_SAMPLES;i++)
		{
			gosAudio_PlayMode sampleStatus = gosAudio_GetChannelPlayMode(i);
			if (sampleStatus == gosAudio_PlayOnce)
			{
				if (fadeDown[i])
				{
					float vol;
					gosAudio_GetChannelSlider(i, gosAudio_Volume, &vol);
					if (vol <= 0.1f)
						vol = 0.1f;

					gosAudio_SetChannelSlider(i, gosAudio_Volume, vol-0.1f);
					gosAudio_GetChannelSlider(i, gosAudio_Volume, &vol);
					if (vol == 0.0f)
					{
						fadeDown[i] = FALSE;
						gosAudio_SetChannelPlayMode(i, gosAudio_Stop);
					}
				}
			}
			else
			{
				fadeDown[i] = FALSE;
				channelSampleId[i] = -1;
				channelInUse[i] = FALSE;
			}
		}
	}

	if (!gamePaused)
		sensorBeepUpdateTime += frameLength;
}

//---------------------------------------------------------------------------
long SoundSystem::playDigitalMusic (long musicId)
{
	//-------------------------------------------------------------------
	// Make sure we have a real music filename.
	if (useMusic)
	{
		if ((musicId >= 0) && (musicId < numDMS) && (strncmp(digitalMusicIds[musicId],"NONE",4) != 0))
		{
			if (musicId == currentMusicId)
				return(-1);
				
			if (useSound)
			{
				//---------------------------------------------------------------------------------------------
				// Just start tune.  No fade necessary.  Set fadeTime to 0.0 to tell update to leave it alone.
				// Use Stream1 by default.
				FullPathFileName digitalStream;
				digitalStream.init(soundPath,digitalMusicIds[musicId],".wav");

				//---------------------------------------------
				// Gotta check if music file is there
				// new sound system can't handle it if its not.
				if (fileExists(digitalStream))
				{
					if (!stream1Active && !stream2Active)
					{
						if (stream1Handle)
						{
							gosAudio_DestroyResource(&stream1Handle);
							stream1Handle = 0;
						}
							
						gosAudio_CreateResource(&stream1Handle, gosAudio_StreamedFile, (const char *) digitalStream);
						
						stream1Time = streamFadeDownTime;
						
						gosAudio_SetChannelSlider( STREAM1CHANNEL, gosAudio_Volume, 0.0f );
						gosAudio_SetChannelSlider( STREAM1CHANNEL, gosAudio_Panning,0.0f );
						gosAudio_AssignResourceToChannel( STREAM1CHANNEL, stream1Handle );

						currentMusicId = musicId;
						stream1Id = musicId;
					}
					else if (!stream2Active)
					{
						if (stream2Handle)
						{
							gosAudio_DestroyResource( &stream2Handle );
							stream2Handle = 0;
						}
							
						gosAudio_CreateResource( &stream2Handle, gosAudio_StreamedFile, (const char *) digitalStream );
						
						//Need to check if stream1 ever got all the way faded up!
						if (stream1Time > 0.0f)
							stream1Time = -stream1Time;
						else
							stream1Time = -streamFadeDownTime;

						stream2Time = streamFadeDownTime;
						
						gosAudio_SetChannelSlider( STREAM2CHANNEL, gosAudio_Volume, 0.0f );
						gosAudio_SetChannelSlider( STREAM2CHANNEL, gosAudio_Panning,0.0f );
						gosAudio_AssignResourceToChannel( STREAM2CHANNEL, stream2Handle );
							
						currentMusicId = musicId;
						stream2Id = musicId;
					}
					else if (!stream1Active)
					{
						if (stream1Handle)
						{
							gosAudio_DestroyResource(&stream1Handle);
							stream1Handle = 0;
						}
							
						gosAudio_CreateResource(&stream1Handle, gosAudio_StreamedFile, (const char *) digitalStream);
						
						stream1Time = streamFadeDownTime;

						//Need to check if stream2 ever got all the way faded up!
						if (stream2Time > 0.0f)
							stream2Time = -stream2Time;
						else
							stream2Time = -streamFadeDownTime;

						gosAudio_SetChannelSlider( STREAM1CHANNEL, gosAudio_Volume, 0.0f );
						gosAudio_SetChannelSlider( STREAM1CHANNEL, gosAudio_Panning,0.0f );
						gosAudio_AssignResourceToChannel( STREAM1CHANNEL, stream1Handle );
							
						currentMusicId = musicId;
						stream1Id = musicId;
					}
					else
					{
						return(-1);
					}
				}
			}
		}
	}

	return(NO_ERR);
}

//---------------------------------------------------------------------------
// PORT (M8-B35): returns the STREAM3 TOKEN this call took, or 0 if it did not
// take the channel -- no such wave on disk, the channel already busy, or sound
// off. The old return said NO_ERR for "started" AND for "there was no file",
// which is exactly the pair a caller has to tell apart before it may stop the
// channel later. Nothing read the old value; all three call sites ignored it.
long SoundSystem::playDigitalStream (const char *streamName)
{
	//-------------------------------------------------------------------
	// Make sure we have a real music filename.
	if (useSound)
	{
		char actualName[1024];
		_splitpath(streamName,NULL,NULL,actualName,NULL);

		//---------------------------------------------------------------------------------------------
		// Just start tune.  No fade necessary.  Set fadeTime to 0.0 to tell update to leave it alone.
		// Use Stream1 by default.
		FullPathFileName digitalStream;
		digitalStream.init(soundPath,actualName,".wav");

		//---------------------------------------------
		// Gotta check if music file is there
		// new sound system can't handle it if its not.
		if (fileExists(digitalStream))
		{
			if (!stream3Active)
			{
				if (stream3Handle)
				{
					gosAudio_DestroyResource(&stream3Handle);
					stream3Handle = 0;
				}
						
				gosAudio_CreateResource(&stream3Handle, gosAudio_StreamedFile, (const char *) digitalStream);
				
				stream3Active = false;
				stream3Token++;
				
				gosAudio_AssignResourceToChannel( STREAM3CHANNEL, stream3Handle );
				gosAudio_SetChannelSlider( STREAM3CHANNEL,gosAudio_Volume, digitalMasterVolume * bettyVolume );
				gosAudio_SetChannelSlider( STREAM3CHANNEL, gosAudio_Panning,0.0f );
				stream3Trace("start scripted", actualName);

				return((long)stream3Token);
			}
			else
			{
				// PORT (M8-B6): this refusal was SILENT, and it is a prime
				// suspect for the owner's report that Colonel Renard's voice
				// lines stop while his video keeps playing (mission 3,
				// 2026-08-16). STREAM3 is shared: the mission script's own
				// CPlayWAV voice lines come through here, and since 0ffd017 a
				// movie's embedded track actually holds this channel. Anything
				// scripted that fires while a movie is talking is dropped with
				// no trace. Measure it before changing the sharing rule.
				if (const char* c = getenv("MC2_AUDIO_CENSUS"))
					if (*c && strcmp(c, "0") != 0)
					{
						fprintf(stderr, "[soundsys] STREAM3 BUSY -- dropped scripted stream '%s'\n",
						        actualName);
						fflush(stderr);
					}
				return(0);
			}
		}
		else
		{
			// The other way this goes quiet: no such wave on disk. Also silent
			// until now, and it looks identical from the outside.
			if (const char* c = getenv("MC2_AUDIO_CENSUS"))
				if (*c && strcmp(c, "0") != 0)
				{
					fprintf(stderr, "[soundsys] no wave file for scripted stream '%s'\n",
					        actualName);
					fflush(stderr);
				}
		}
	}

	// Nothing was started, so the caller holds no claim on STREAM3.
	return(0);
}

//---------------------------------------------------------------------------
// PORT (M8-1): playDigitalStream's twin for sound that is already decoded and
// in memory -- a movie's own Opus track, unpacked to 16-bit PCM. Everything
// downstream is deliberately identical: same STREAM3 channel, same volume, and
// the same update() that flips it to PlayOnce and destroys the resource when it
// finishes, so stopSupportSample() still stops a movie exactly as before.
//
// gosAudio_UserMemory stores the POINTER (audio_common.cpp:308). `pcm` must
// therefore outlive the resource, which is why MC2Movie stops this channel in
// its destructor before releasing the buffer.
long SoundSystem::playDigitalMemory (void *pcm, long bytes, long channels, long samplesPerSecond,
									 float volumeScale)
{
	if (!useSound || !pcm || bytes <= 0 || channels <= 0 || samplesPerSecond <= 0)
		return(-1);

	if (stream3Active)
	{
		stream3Trace("BUSY -- dropped movie track", "");
		return(-1);
	}

	if (stream3Handle)
	{
		gosAudio_DestroyResource(&stream3Handle);
		stream3Handle = 0;
	}

	gosAudio_Format format;
	memset(&format,0,sizeof(format));
	format.wFormatTag		= 1;					//PCM
	format.nChannels		= (WORD)channels;
	format.nSamplesPerSec	= (DWORD)samplesPerSecond;
	format.wBitsPerSample	= 16;
	format.nBlockAlign		= (WORD)(channels * 2);
	format.nAvgBytesPerSec	= (DWORD)(samplesPerSecond * channels * 2);
	format.cbSize			= 0;

	gosAudio_CreateResource(&stream3Handle, gosAudio_UserMemory, "movie audio",
							&format, pcm, (int)bytes);

	stream3Active = false;
	stream3Token++;

	// The slider tops out at 1.0, so a scale that would exceed it is clamped
	// rather than wrapped. MC2Movie already spends as much of the requested
	// boost as it can on the samples themselves and only sends the remainder
	// here, so this clamp should not bite -- at the owner's settings the
	// unscaled value is 0.397, leaving 2.5x of room.
	float channelVolume = digitalMasterVolume * bettyVolume * volumeScale;
	if (channelVolume > 1.0f)
		channelVolume = 1.0f;

	gosAudio_AssignResourceToChannel( STREAM3CHANNEL, stream3Handle );
	gosAudio_SetChannelSlider( STREAM3CHANNEL, gosAudio_Volume, channelVolume );
	gosAudio_SetChannelSlider( STREAM3CHANNEL, gosAudio_Panning, 0.0f );
	stream3Trace("start movie track", "");

	// PORT (M8-B35): the token, matching playDigitalStream. Anything <= 0 means
	// this call did not take the channel.
	return((long)stream3Token);
}

//---------------------------------------------------------------------------
long SoundSystem::playBettySample (unsigned long bettySampleId)
{
	if (useSound && (bettySoundBite == NULL))	//Playing Betty takes precedence
	{

		if (bettySampleId >= numBettySamples)
			return(-1);
			
		long ourChannel = BETTY_CHANNEL;
	
		gosAudio_SetChannelSlider( ourChannel, gosAudio_Panning, 0.0f );
		float vol = 1.0;
		gosAudio_SetChannelSlider( ourChannel, gosAudio_Volume, digitalMasterVolume * vol * bettyVolume );
		gosAudio_SetChannelSlider( RADIO_CHANNEL, gosAudio_Volume, digitalMasterVolume * vol * radioVolume * 0.5f ); 
		lastBettyId = bettySampleId;
		channelInUse[ourChannel] = TRUE;


		long result = bettyDataFile->seekPacket(bettySampleId);
		if (result != NO_ERR)
			return(-1);
			
		long bettySize = bettyDataFile->getPacketSize();
		bettySoundBite = (MemoryPtr)soundHeap->Malloc(bettySize);
		gosASSERT(bettySoundBite != NULL);
		
		bettyDataFile->readPacket(bettySampleId,bettySoundBite);
		
		//--------------------------------------------------------------------
		// Hand GOS sound the data it needs to create the resource Handle
		gosAudio_Format soundFormat;
		soundFormat.wFormatTag = 1;				//PCM
		
		MC2_WAVEFORMATEX *waveFormat = NULL;
		MemoryPtr dataOffset = NULL;
		DWORD length = 0;
		DWORD bitsPerSec = 0;
		wave_ParseWaveMemory(bettySoundBite,&waveFormat,&dataOffset,&length);
		if (!waveFormat) waveFormat = wave_EmptyFormat();	// PORT: parse can fail and leave this NULL
		
		bitsPerSec = waveFormat->nBlockAlign / waveFormat->nChannels * 8;

		soundFormat.nChannels = waveFormat->nChannels;
		soundFormat.nSamplesPerSec = waveFormat->nSamplesPerSec;
		soundFormat.nAvgBytesPerSec = waveFormat->nAvgBytesPerSec;
		soundFormat.nBlockAlign = waveFormat->nBlockAlign;
		soundFormat.wBitsPerSample = bitsPerSec;
		soundFormat.cbSize = 0;

		gosAudio_CreateResource(&bettyHandle,gosAudio_UserMemory, NULL, &soundFormat,dataOffset,length);
		gosAudio_AssignResourceToChannel( ourChannel, bettyHandle );
		gosAudio_SetChannelPlayMode(ourChannel, gosAudio_PlayOnce);
			
		return(BETTY_CHANNEL);
	}

	return(-1);
}

//---------------------------------------------------------------------------
long SoundSystem::playSupportSample (unsigned long supportSampleId, char* fileName)
{
	if (useSound && (supportSoundBite == NULL))		//Playing Support takes precedence
	{
		if (!fileName)
		{
			if (supportSampleId >= numSupportSamples)
				return(-1);
		}
			
		long ourChannel = SUPPORT_CHANNEL;
		
		if (fileName) 
		{
			playDigitalStream(fileName);
			return (STREAM3CHANNEL);
		}
		else 
		{
	
			gosAudio_SetChannelSlider( ourChannel, gosAudio_Panning, 0.0f );
			float vol = 1.0;
			gosAudio_SetChannelSlider( ourChannel, gosAudio_Volume, digitalMasterVolume * vol * bettyVolume );
			lastSupportId = supportSampleId;
			channelInUse[ourChannel] = TRUE;

			long result = supportDataFile->seekPacket(supportSampleId);
			if (result != NO_ERR)
				return(-1);
			long supportSize = supportDataFile->getPacketSize();
			if (supportSize > 0)
			{
				supportSoundBite = (MemoryPtr)soundHeap->Malloc(supportSize);
				gosASSERT(supportSoundBite != NULL);
			}
			else
			{
				return -1;      //Sound Bite does NOT exist!! 
			}
			
			supportDataFile->readPacket(supportSampleId,supportSoundBite);
		}
		
		//--------------------------------------------------------------------
		// Hand GOS sound the data it needs to create the resource Handle
		gosAudio_Format soundFormat;
		soundFormat.wFormatTag = 1;				//PCM
		
		MC2_WAVEFORMATEX *waveFormat = NULL;
		MemoryPtr dataOffset = NULL;
		DWORD length = 0;
		DWORD bitsPerSec = 0;
		wave_ParseWaveMemory(supportSoundBite,&waveFormat,&dataOffset,&length);
		if (!waveFormat) waveFormat = wave_EmptyFormat();	// PORT: parse can fail and leave this NULL
		
		bitsPerSec = waveFormat->nBlockAlign / waveFormat->nChannels * 8;

		soundFormat.nChannels = waveFormat->nChannels;
		soundFormat.nSamplesPerSec = waveFormat->nSamplesPerSec;
		soundFormat.nAvgBytesPerSec = waveFormat->nAvgBytesPerSec;
		soundFormat.nBlockAlign = waveFormat->nBlockAlign;
		soundFormat.wBitsPerSample = bitsPerSec;
		soundFormat.cbSize = 0;

		gosAudio_CreateResource(&supportHandle,gosAudio_UserMemory, NULL, &soundFormat,dataOffset,length);
		gosAudio_AssignResourceToChannel( ourChannel, supportHandle );
		gosAudio_SetChannelPlayMode(ourChannel, gosAudio_PlayOnce);
			
		return(SUPPORT_CHANNEL);
	}

	return(-1);
}

//---------------------------------------------------------------------------
bool SoundSystem::isPlayingSample (long sampleId)
{
	long i;
	for (i=0;i<MAX_DIGITAL_SAMPLES;i++)
	{
		if ((sampleId == channelSampleId[i]))
			return TRUE;
	}

	return FALSE;
}

//---------------------------------------------------------------------------
bool SoundSystem::isChannelPlaying (long channelId)
{
	if ((channelId < 0) || (channelId > MAX_DIGITAL_SAMPLES))
		return(FALSE);
		
	gosAudio_PlayMode sampleStatus = gosAudio_GetChannelPlayMode(channelId);
	if (sampleStatus == gosAudio_PlayOnce || sampleStatus == gosAudio_Loop)
		return(TRUE);

	return FALSE;
}

//---------------------------------------------------------------------------
bool SoundSystem::isPlayingVoiceOver (void)
{
	if (stream3Handle)
		return true;

	return false;
}

//---------------------------------------------------------------------------
bool SoundSystem::isDigitalMusicPlaying (void)
{
	if (stream2Handle || stream1Handle)
		return true;

	return false;
}

//---------------------------------------------------------------------------
long SoundSystem::findOpenChannel (long start, long end)
{
	long channel = start;
	
	while (channelInUse[channel] && (channel < end))
		channel++;
		
	if (channel == end)
		return(-1);
		
	return(channel);
}	

//---------------------------------------------------------------------------
long SoundSystem::playDigitalSample (unsigned long sampleId, Stuff::Vector3D pos, bool allowDupes)
{
	if (useSound && allowDupes || (!isPlayingSample(sampleId) && !allowDupes))
	{
		if (sampleId >= numSoundBites)
			return(-1);
			
		long ourChannel = findOpenChannel(1,SUPPORT_CHANNEL);
	
		if (ourChannel != -1)
		{
			float distanceVolume = 1.0f;
			float panVolume = 0.0f;
			if (eye && (pos.z != -9999.0f))
			{
				Stuff::Vector3D distance;
				distance.Subtract(eye->getPosition(),pos);
				float dist = distance.GetApproximateLength();
				if (dist < FALLOFF_DISTANCE)
					distanceVolume = (FALLOFF_DISTANCE - dist) / FALLOFF_DISTANCE;
				else
					return -1;		//Do not play sound.  TOO far away!!

				//Figure out where in stereo field to play.
				OppRotate(distance,eye->getRotation().y);

				panVolume = distance.x / (FALLOFF_DISTANCE * 0.5f);
				if (panVolume > 1.0f)
					panVolume = 1.0f;
				else if (panVolume < -1.0f)
					panVolume = -1.0f;
			}

			float vol = sounds[sampleId].volume * distanceVolume;
			if (vol > 1.0f)
				vol = 1.0f;
			else if (vol <= 0.0f)		//No VOlume.  DON't PLAY!
				return -1;

			gosAudio_SetChannelSlider(ourChannel,gosAudio_Panning, panVolume);
			gosAudio_SetChannelSlider(ourChannel,gosAudio_Volume, (digitalMasterVolume * vol * SFXVolume)) ;
			channelSampleId[ourChannel] = sampleId;
			channelInUse[ourChannel] = TRUE;
				
			if (sounds[sampleId].biteData && sounds[sampleId].resourceHandle)
			{
				gosAudio_AssignResourceToChannel( ourChannel, sounds[sampleId].resourceHandle );
				gosAudio_SetChannelPlayMode(ourChannel, gosAudio_PlayOnce);
			}
			
			return(ourChannel);
		}
	}

	return(-1);
}

//---------------------------------------------------------------------------
void SoundSystem::stopDigitalSample (unsigned long sampleHandleNumber)
{
	if (useSound)
	{
		gosAudio_PlayMode
			sampleStatus = gosAudio_GetChannelPlayMode(sampleHandleNumber);
		if ( sampleStatus == gosAudio_PlayOnce || sampleStatus == gosAudio_Loop )
		{
			fadeDown[sampleHandleNumber] = TRUE;
		}
	}
}

//---------------------------------------------------------------------------
void SoundSystem::stopDigitalMusic (void)
{
	if (useSound && useMusic)
	{
		if (stream1Active && stream1Handle)
		{
			if (stream1Handle) 
				gosAudio_DestroyResource(&stream1Handle);
				
			stream1Handle = NULL;
			stream1Time = 0.0;
			stream1Active = FALSE;
		}
		
		if (stream2Active && stream2Handle)
		{
			if (stream2Handle) 
				gosAudio_DestroyResource(&stream2Handle);
				
			stream2Handle = NULL;
		 	stream2Time = 0.0;
		 	stream2Active = FALSE;
		}

		currentMusicId = -1;
	}
}	

//---------------------------------------------------------------------------
void SoundSystem::stopSupportSample (const char* why, unsigned long token)
{
	// PORT (M8-B28): a caller whose own audio has already been superseded must
	// not silence whoever holds the channel now. See Soundsys.h for the log
	// lines that made this necessary.
	if (token && token != stream3Token)
	{
		stream3Trace("stop IGNORED, not the owner --", why);
		return;
	}

	if (stream3Handle)
	{
		stream3Trace("STOPPED by", why);
		gosAudio_DestroyResource(&stream3Handle);
		stream3Handle = 0;
		stream3Active = false;
	}
}

//---------------------------------------------------------------------------
void SoundSystem::stopBettySample (void)
{
	if (bettyHandle)
	{
			gosAudio_SetChannelPlayMode(BETTY_CHANNEL, gosAudio_Stop);
			gosAudio_DestroyResource(&bettyHandle);
			soundHeap->Free(bettySoundBite);
			bettySoundBite = NULL;
			bettyHandle = NULL;


	}
}


//---------------------------------------------------------------------------
void SoundSystem::setDigitalMasterVolume (byte volume)
{
	if (useSound)
	{
		DigitalMasterVolume = volume;
		digitalMasterVolume = float(DigitalMasterVolume) / 256.0f;
	}
}

//---------------------------------------------------------------------------
long SoundSystem::getDigitalMasterVolume (void)
{
	if (useSound)
	{
		return DigitalMasterVolume;
	}
	
	return(0);
}	

//---------------------------------------------------------------------------
void SoundSystem::setSFXVolume(unsigned char volume)
{
	if (useSound)
	{
		sfxVolume = volume;
		SFXVolume = float(sfxVolume) / 256.0f;
	}
}

//---------------------------------------------------------------------------
unsigned char SoundSystem::getSFXVolume(void)
{
	if (useSound)
	{
		return sfxVolume;
	}
	
	return(0);
}	

//---------------------------------------------------------------------------
void SoundSystem::setRadioVolume(unsigned char volume)
{
	if (useSound)
	{
		RadioVolume = volume;
		radioVolume = float(RadioVolume) / 256.0f;
	}
}

//---------------------------------------------------------------------------
unsigned char SoundSystem::getRadioVolume(void)
{
	if (useSound)
	{
		return RadioVolume;
	}
	
	return(0);
}	

//---------------------------------------------------------------------------
void SoundSystem::setMusicVolume(unsigned char volume)
{
	if (useSound)
	{
		MusicVolume = volume;
		musicVolume = float(MusicVolume) / 256.0f;
		if (useMusic)
		{
			if (stream1Active && stream1Handle)
			{
				gosAudio_SetChannelSlider( STREAM1CHANNEL,gosAudio_Volume, (digitalMasterVolume * musicVolume) );
			}
			if (stream2Active && stream2Handle)
			{
				gosAudio_SetChannelSlider( STREAM2CHANNEL,gosAudio_Volume, (digitalMasterVolume * musicVolume) );
			}
		}
	}
}

//---------------------------------------------------------------------------
unsigned char SoundSystem::getMusicVolume(void)
{
	if (useSound)
	{
		return MusicVolume;
	}
	
	return(0);
}	

//---------------------------------------------------------------------------
void SoundSystem::setBettyVolume(unsigned char volume)
{
	if (useSound)
	{
		BettyVolume = volume;
		bettyVolume = float(BettyVolume) / 256.0f;
	}
}

//---------------------------------------------------------------------------
unsigned char SoundSystem::getBettyVolume(void)
{
	if (useSound)
	{
		return BettyVolume;
	}
	
	return(0);
}	

//---------------------------------------------------------------------------
void SoundSystem::stopABLMusic (void)
{
	stopDigitalMusic();
}	

//---------------------------------------------------------------------------
void SoundSystem::playABLSFX (long sfxId)
{
	playDigitalSample(sfxId);
}	

//---------------------------------------------------------------------------
void SoundSystem::playABLDigitalMusic (long musicId)
{
//	PAUSE(("Switching to Tune %d.  Playing Tune %d.",musicId,currentMusicId));
	playDigitalMusic(musicId);
}	

//---------------------------------------------------------------------------
