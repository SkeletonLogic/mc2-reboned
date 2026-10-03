//------------------------------------------------------------------
//
// Movie class
//
//Notes:
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef MC2MOVIE_H
#define MC2MOVIE_H

//--------------------------------------------------------------------------
#ifndef DSTD_H
#include "dstd.h"
#endif

#ifndef HEAP_H
#include "Heap.h" // PORT: match case-sensitive host path.
#endif

#include <windows.h>

//--------------------------------------------------------------------------
// PORT (M8-1): the decoder lives behind gameos/impl/video/video_decoder.h, so
// no game header depends on ffmpeg. Swapping backends -- or building with none
// at all, which is the default -- touches nothing in game code.
namespace mc2video { class Decoder; }

//--------------------------------------------------------------------------
const DWORD MAX_TEXTURES_NEEDED = 6;

//--------------------------------------------------------------------------
class MC2Movie
{
	public:
		MC2Movie (void)
		{
			MC2Surface = NULL;

			for (long i=0;i<MAX_TEXTURES_NEEDED;i++)
			{
				mc2TextureNodeIndex[i] = 0xffffffff;
			}

			MC2Rect.bottom = MC2Rect.top = MC2Rect.left = MC2Rect.right = 0;

			numWide = numHigh = 0;
			totalTexturesUsed = 0;

			forceStop = false;
			stillPlaying = false;

			separateWAVE = false;
			soundStarted = false;

			waveName = NULL;
			m_MC2Name = NULL;

			// PORT (M8-1)
			m_decoder = NULL;
			m_startTime = 0.0;
			m_texturesReady = false;
			m_frameWidth = m_frameHeight = 0;
			m_audioPcm = NULL;
			m_audioSamples = 0;
			m_audioStarted = false;
			m_streamToken = 0;		// PORT (M8-B35): holds no channel yet.
			m_audioRefusals = 0;
			m_reportedRender = false;
			m_audioGain = 1.0f;
			m_residualGain = 1.0f;
			m_paused = false;
			m_pausedAt = 0.0;
		}

		~MC2Movie (void)
		{
			releaseDecoder();	// PORT (M8-1): also frees the movie's textures and stops its sound.

			if (MC2Surface)
			{
				systemHeap->Free(MC2Surface);
				MC2Surface = NULL;
			}

			if (waveName)
			{
				delete [] waveName;
				waveName = NULL;
			}

			if (m_MC2Name)
			{
				delete [] m_MC2Name;
				m_MC2Name = NULL;
			}
		}

		//Movie name assumes path is correct.
		// Sets up the MC2 to be played.
		//
		// PORT (M8-B5): audioGain scales the movie's OWN soundtrack, owner ruling
		// 2026-08-16 -- "double the volume of the in-game comms movies and the
		// mission select movies". Only those two callers pass 2.0f; the intro and
		// the full-screen cinemas keep retail level, and they could not take a
		// boost anyway (measured: cinema1-5 already peak at 0.0 dB).
		//
		// The gain is capped per movie by that movie's own headroom, so asking
		// for 2.0 never distorts a track that has less than 6 dB spare. It is
		// applied to the decoded samples rather than to the channel slider,
		// because the slider is clamped at 1.0 and would silently stop doubling
		// once the player's master volume is high enough.
		void init (char *MC2Name, RECT mRect, bool useWaveFile, float audioGain = 1.0f);

		//Handles tickling MC2 to make sure we keep playing back
		// Returns true when MC2 is DONE playing!!
		bool update (void);

		//Actually draws the MC2 texture using gos_DrawTriangle.
		void render (void);

		//Immediately stops playback of MC2.
		void stop (void)
		{
			forceStop = true;
		}

		//Pause video playback.
		// PORT (M8-B4): this was a stub that only cleared forceStop, which did
		// nothing at all once playback became real -- update() is called every
		// frame by MissionSelectionScreen whether "paused" or not, so a paused
		// movie carried on playing. The clock is frozen instead.
		void pause (bool pauseState);

		//Restarts MC2 from beginning.  Can be called anytime.
		// PORT (M8-B4): "from beginning" now means it -- clock, decoder and
		// sound. Setting the two flags and nothing else left a restarted movie
		// racing to whatever frame the wall clock had reached, with its sound
		// already spent. That is exactly the briefing-screen case: the movie is
		// stopped at init and only ever started by the player pressing play.
		void restart (void);

		//Changes rect.  If resize, calls malloc which will be QUITE painful during a MC2 playback
		// If just move, its awfully fast.
		void setRect (RECT vRect);

		bool isPlaying (void)
		{
			return stillPlaying;
		}

		char *getMovieName (void)
		{
			return m_MC2Name;
		}

		//------------------------------------------------------------------
		// PORT (M8-1): existence test for callers that gate on the movie file
		// before they build one of these. They ask for the retail ".bik" name;
		// what is actually on disk is the converted ".webm", and there are ZERO
		// .bik files in the run directory -- so a caller testing the name it was
		// handed always answers "no movie" and never reaches init(). Both the
		// preferred name and the one asked for are tried here, through exactly
		// the same resolution init() uses.
		static bool mediaExists (const char *movieName);

		// Fills `out` with the name init() will actually try first: the given
		// path with its extension replaced by ".webm".
		static void preferredName (const char *movieName, char *out, size_t outSize);

	protected:
	
		DWORD*		MC2Surface;									//Extra surface used if MC2 Movie is larger then 256x256
		DWORD		mc2TextureNodeIndex[MAX_TEXTURES_NEEDED];		//Handles to textures for MC2 movie data.
		RECT		MC2Rect;										//Physical Location on screen for MC2 movie.
					
		DWORD 		numWide;										//Number of textures wide display is
		DWORD		numHigh;										//Number of textures high the display is
		DWORD		totalTexturesUsed;                              //total Number of texture used to display 
					
		bool		forceStop;										//Should MC2 movie end now?
					
		DWORD		singleTextureSize;								//Size of the single texture.  Fit it to smallest texture we can use.
		bool		stillPlaying;									//Is MC2 movie over?

		bool		separateWAVE;									//Tells us if this MC2 movie has a separate soundtrack.
		bool		soundStarted;									//If this MC2 movie has a separate soundtrack, this tells us when to start it.
		char		*waveName;										//Name of the wavefile.
		char		*m_MC2Name;										//Name of the Movie.

		void BltMovieFrame (void);								//Actually moves frame data from MC2 movie to surface and/or texture(s)

		// PORT (M8-1): decoder state. mc2TextureNodeIndex above now holds GOS
		// texture handles from gos_NewEmptyTexture rather than texture-manager
		// node indices -- these are written every frame and never cached out,
		// so they do not belong to the manager's cache.
		mc2video::Decoder*	m_decoder;
		double				m_startTime;							//Real-time clock reading when playback began.
		bool				m_texturesReady;
		long				m_frameWidth;
		long				m_frameHeight;

		// PORT (M8-1): the movie's own soundtrack, decoded whole at init.
		// gosAudio_UserMemory keeps the POINTER, so this buffer must outlive
		// the resource -- releaseMovieAudio() stops the channel first, then
		// frees, and never the other way round.
		short				*m_audioPcm;
		long				m_audioSamples;							//Count of 16-bit samples, all channels interleaved.
		bool				m_audioStarted;							//Has the buffer been handed to a channel yet?
		// PORT (M8-B35): the STREAM3 token this movie holds, from whichever of
		// the two sound paths actually started -- 0 when it holds nothing. A
		// movie with no track of its own and no sidecar wave on disk lands here
		// at 0 and must not stop the channel: 109 of the 185 retail movies
		// declare zero audio tracks and none of them ships a sidecar, so every
		// pilot chatter video used to kill whatever scripted voice line was
		// talking when it finished. Measured in the owner's 2026-09-04 log: the
		// pilot video 'meat1' cut mission 15's "Rebel Blow Up Sensor" line
		// (mc2_15_4, 4.83 s) about halfway through.
		unsigned long		m_streamToken;
		long				m_audioRefusals;						//Times STREAM3 turned it away; census only.
		bool				m_reportedRender;						//One render census line per movie, not per session.
		bool				m_paused;
		double				m_pausedAt;								//Movie time held while paused.

		float				m_audioGain;							//Requested boost for this movie's own track.
		float				m_residualGain;							//The part of it the samples could not carry; goes to the channel.

		void releaseDecoder (void);
		void prepareMovieAudio (bool useWaveFile);				//Decode the track at init; does not play it.
		void applyAudioGain (void);								//Owner's 2x for comms/mission-select, capped by headroom.
		void tryStartMovieAudio (void);							//Hand it to STREAM3 once playback is really running.
		void releaseMovieAudio (void);
		void stopMovieSound (void);								//Silences whichever of the two sound paths this movie uses.
		bool audioStillOwed (double elapsed);					//Own track has time left; do not report done yet.
};

typedef MC2Movie *MC2MoviePtr;
//--------------------------------------------------------------------------
#endif