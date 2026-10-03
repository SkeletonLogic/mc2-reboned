//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef MC2MOVIE_H
#include "mc2movie.h"
#endif

#ifndef TXMMGR_H
#include "txmmgr.h"
#endif

#ifndef FILE_H
#include "file.h"
#endif

#ifndef GAMESOUND_H
#include "gamesound.h"
#endif

#ifndef PREFS_H
#include "prefs.h"
#endif

#include "../resource.h" // PORT: use host-independent include separators.
#include <GameOS.HPP> // PORT: match case-sensitive host path.

// PORT (M8-1): video playback. Retail called Bink here; Microsoft stripped that
// decoder before releasing the source, which is why every function below was an
// empty shell. Everything now goes through the backend-agnostic seam.
#include "../../gameos/impl/video/video_decoder.h"
#include <math.h>

//-----------------------------------------------------------------------
const DWORD MAX_TEXTURE_WIDTH 	= 256;
const DWORD MAX_TEXTURE_HEIGHT 	= 256;
const DWORD MAX_MOVIE_WIDTH 	= 640;
const DWORD MAX_MOVIE_HEIGHT 	= 480;
const float TEXTURE_ADJUST_MIN	= (0.4f / MAX_TEXTURE_WIDTH);
const float TEXTURE_ADJUST_MAX	= (1.0f - TEXTURE_ADJUST_MIN);

float averageFrameRate = 0.0f;
long currentFrameNum = 0;
float last30Frames[30] = {
	0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 0.0f, 0.0f, 0.0f
};

extern char CDInstallPath[];
void EnterWindowMode();
void EnterFullScreenMode();
void __stdcall ExitGameOS();

//-----------------------------------------------------------------------
// PORT (M8-1): the callers build a ".bik" path because that is what retail
// shipped. The converted set is ".webm" (VP9 + Opus), so that is what gets
// tried first, everywhere -- both here and in the existence tests two callers
// run BEFORE they build a movie.
void MC2Movie::preferredName (const char *movieName, char *out, size_t outSize)
{
	if (!out || outSize == 0)
		return;

	out[0] = 0;
	if (!movieName)
		return;

	strncpy(out,movieName,outSize-1);
	out[outSize-1] = 0;

	char* extension = strrchr(out,'.');
	if (extension && (strlen(out) - (extension - out)) <= 5)
	{
		// Room for ".webm" where a shorter extension used to be.
		if ((size_t)(extension - out) + 6 <= outSize)
			strcpy(extension,".webm");
	}
	else if (strlen(out) + 6 <= outSize)
	{
		strcat(out,".webm");
	}
}

//-----------------------------------------------------------------------
bool MC2Movie::mediaExists (const char *movieName)
{
	if (!movieName || !movieName[0])
		return false;

	// Nothing can be played without a decoder, so do not claim a movie exists
	// just because its file does -- that is how a caller ends up drawing an
	// empty video panel it then has to tear down.
	if (!mc2video::DecoderAvailable())
		return false;

	char preferred[1024];
	preferredName(movieName,preferred,sizeof(preferred));
	if (preferred[0] && fileExists(preferred))
		return true;

	return fileExists((char *)movieName) != 0;
}

//-----------------------------------------------------------------------
// Class MC2Movie
void MC2Movie::init (char *MC2Name, RECT mRect, bool useWaveFile, float audioGain)
{
	m_audioGain = audioGain;

	char MOVIEName[1024];
	_splitpath(MC2Name,NULL,NULL,MOVIEName,NULL);

	m_MC2Name = new char [strlen(MOVIEName)+1];
	memset(m_MC2Name,0,strlen(MOVIEName)+1);
	strcpy(m_MC2Name,MOVIEName);

	MC2Rect = mRect;

	// Retail's separate-wave setup runs FIRST and unconditionally, exactly as
	// it did before a decoder existed. That matters for the build with no
	// decoder at all: returning early below without it silently dropped the
	// soundtrack these movies have always played. If the movie turns out to
	// carry its own track, startMovieAudio() cancels this.
	if (useWaveFile && (prefs.DigitalMasterVolume != 0.0f))
	{
		separateWAVE = true;
		soundStarted = false;

		waveName = new char [strlen(MOVIEName)+1];
		memset(waveName,0,strlen(MOVIEName)+1);
		strcpy(waveName,MOVIEName);
	}

	char playPath[1024];
	preferredName(MC2Name,playPath,sizeof(playPath));

	m_decoder = mc2video::CreateDecoder();
	bool opened = m_decoder && m_decoder->Open(playPath);
	if (!opened && m_decoder)
		opened = m_decoder->Open(MC2Name);		//Retail .bik, if a decoder can read it.

	if (!opened)
	{
		// No decoder, or no file: behave exactly as this class has since the
		// Bink code was removed -- report not-playing and let the caller drop us.
		releaseDecoder();
		stillPlaying = false;
		return;
	}

	m_frameWidth  = m_decoder->Width();
	m_frameHeight = m_decoder->Height();

	// One square texture per tile, sized to the smallest power of two that
	// covers the frame, capped at 256 because that is the contract this class
	// was written against (MAX_TEXTURE_WIDTH/HEIGHT).
	singleTextureSize = 4;
	while ((singleTextureSize < (DWORD)m_frameWidth || singleTextureSize < (DWORD)m_frameHeight)
			&& singleTextureSize < MAX_TEXTURE_WIDTH)
		singleTextureSize <<= 1;

	numWide = (m_frameWidth  + singleTextureSize - 1) / singleTextureSize;
	numHigh = (m_frameHeight + singleTextureSize - 1) / singleTextureSize;
	totalTexturesUsed = numWide * numHigh;
	if (totalTexturesUsed > MAX_TEXTURES_NEEDED)
	{
		// Nothing in the retail set reaches this -- the largest movie is
		// 400x300 -- but a bad file must not scribble past the handle array.
		releaseDecoder();
		stillPlaying = false;
		return;
	}

	for (DWORD i = 0; i < totalTexturesUsed; i++)
		mc2TextureNodeIndex[i] = gos_NewEmptyTexture(gos_Texture_Solid,"MC2 movie frame",singleTextureSize,0);
	m_texturesReady = true;

	prepareMovieAudio(useWaveFile);

	m_startTime = gos_GetElapsedTime(1);
	stillPlaying = true;
	forceStop = false;

	// PORT (M8-B1): the owner reports the intro and new-campaign movies playing
	// but mis-sized and mis-aligned. The callers build their rect in MIXED
	// units -- logistics.cpp:910 uses Environment.screenWidth for `right`
	// (physical, 1920) alongside top=100/bottom=500, which are authored 800x600
	// canvas numbers that agreed with each other only at retail's 800x600. What
	// is NOT yet known is which space this quad is interpreted in, so both the
	// rect as handed over and the screen it is measured against are reported
	// rather than assumed. Armed with MC2_VIDEO_CENSUS=1.
	if (getenv("MC2_VIDEO_CENSUS"))
	{
		fprintf(stderr, "[movie] '%s' rect l=%ld t=%ld r=%ld b=%ld (%ldx%ld) "
		                "screen=%ldx%ld frame=%ldx%ld tiles=%lux%lu size=%lu\n",
		        m_MC2Name ? m_MC2Name : "?",
		        (long)MC2Rect.left, (long)MC2Rect.top,
		        (long)MC2Rect.right, (long)MC2Rect.bottom,
		        (long)(MC2Rect.right - MC2Rect.left),
		        (long)(MC2Rect.bottom - MC2Rect.top),
		        (long)Environment.screenWidth, (long)Environment.screenHeight,
		        (long)m_frameWidth, (long)m_frameHeight,
		        (unsigned long)numWide, (unsigned long)numHigh,
		        (unsigned long)singleTextureSize);
		fflush(stderr);
	}
}

//-----------------------------------------------------------------------
// PORT (M8-1): the movie's own soundtrack.
//
// The owner ruled on 2026-08-15 that movie sound lives INSIDE the movie file:
// 54 of the 76 movies with audio also have a same-named file in data\sound, and
// playing both gives a doubled, slightly offset echo. So the separate wave set
// up in init() is cancelled here, and ONLY when this movie's own track has
// actually been decoded -- if the decode yields nothing, the wave stands.
//
// Whole-track preload rather than streaming, because the sink decides: a
// gosAudio_UserMemory resource is one pointer to PCM that has to stay valid
// (audio_common.cpp:308), with nothing to push samples into frame by frame.
//
// DECODING happens here, at init. PLAYING does not -- see tryStartMovieAudio.
void MC2Movie::prepareMovieAudio (bool useWaveFile)
{
	// PORT (M8-B4): every early return says WHY under the census. The device
	// log proved the sound never arrived; it could not say which of these
	// stages dropped it, and guessing between them is what this avoids.
	const bool census = getenv("MC2_VIDEO_CENSUS") != NULL;
	#define MOVIE_AUDIO_DECLINE(reason)                                        \
		do {                                                                   \
			if (census) {                                                      \
				fprintf(stderr, "[movie] '%s' no own audio: %s\n",             \
				        m_MC2Name ? m_MC2Name : "?", reason);                  \
				fflush(stderr);                                                \
			}                                                                  \
			return;                                                            \
		} while (0)

	if (!m_decoder)
		MOVIE_AUDIO_DECLINE("no decoder");
	if (!useWaveFile)
		MOVIE_AUDIO_DECLINE("caller passed useWaveFile=false");
	if (prefs.DigitalMasterVolume == 0.0f)
		MOVIE_AUDIO_DECLINE("DigitalMasterVolume is 0");

	if (m_decoder->AudioChannels() <= 0)
		MOVIE_AUDIO_DECLINE("file carries no track");	//109 of the 185 movies.

	const int available = m_decoder->PreloadAudio();
	if (available <= 0)
		MOVIE_AUDIO_DECLINE("track declared but decoded to 0 samples");

	m_audioPcm = new short [available];
	m_audioSamples = m_decoder->PopAudio(m_audioPcm,available);
	if (m_audioSamples <= 0)
	{
		delete [] m_audioPcm;
		m_audioPcm = NULL;
		m_audioSamples = 0;
		MOVIE_AUDIO_DECLINE("PopAudio returned nothing");
	}
	#undef MOVIE_AUDIO_DECLINE

	applyAudioGain();

	//This movie speaks for itself.  Silence the sidecar wave.
	if (separateWAVE)
	{
		separateWAVE = false;
		soundStarted = false;
		if (waveName)
		{
			delete [] waveName;
			waveName = NULL;
		}
	}

	if (getenv("MC2_VIDEO_CENSUS"))
	{
		fprintf(stderr, "[movie] '%s' audio prepared: %ld samples %dch %dHz\n",
		        m_MC2Name ? m_MC2Name : "?", m_audioSamples,
		        m_decoder->AudioChannels(), m_decoder->AudioSampleRate());
		fflush(stderr);
	}
}

//-----------------------------------------------------------------------
// PORT (M8-B5): owner ruling 2026-08-16 -- the in-game comms and mission-select
// movies play at double volume. Those two callers pass audioGain 2.0f.
//
// WHY THE SAMPLES AND NOT THE CHANNEL SLIDER. gosAudio_Volume is a 0..1 value.
// At the owner's settings the movie channel already sits at 0.3969, so 2x fits
// -- but a player with the master volume higher would land above 1.0, be
// clamped, and quietly get less than the doubling they were promised. Scaling
// the decoded PCM is independent of every slider.
//
// WHY IT IS CAPPED. Measured across all 76 movies that carry a track: node_04,
// a mission-select video, peaks at -9.7 dB and has room to spare, which is why
// these sounded thin against battle in the first place. But 19 of the 76 have
// under 6 dB of headroom -- the five cinemas and msft are already at 0.0 dB, and
// w01..w12 sit near -1 dB. Doubling those would clip on every peak. So the gain
// asked for is reduced to whatever that particular movie can actually take, and
// the census reports when it was.
void MC2Movie::applyAudioGain (void)
{
	if (!m_audioPcm || m_audioSamples <= 0 || m_audioGain <= 1.0f)
		return;

	long peak = 0;
	for (long i = 0; i < m_audioSamples; i++)
	{
		long magnitude = m_audioPcm[i] < 0 ? -(long)m_audioPcm[i] : (long)m_audioPcm[i];
		if (magnitude > peak)
			peak = magnitude;
	}

	if (peak <= 0)
		return;

	float gain = m_audioGain;
	const float headroom = 32767.0f / (float)peak;
	if (gain > headroom)
		gain = headroom;

	// Whatever the samples cannot carry is handed to the channel slider, which
	// has its own room: at the owner's settings the movie channel sits at
	// 0.3969, so there is 2.5x spare before it clamps. Measured 2026-08-16 --
	// every one of the 27 comms movies peaks between -8.5 and -10.6 dB, giving
	// 2.7x to 3.4x in the samples alone, so a 3.5x request NEEDS this split.
	m_residualGain = (gain > 0.0f) ? (m_audioGain / gain) : 1.0f;

	if (gain > 1.0f)
	{
		for (long i = 0; i < m_audioSamples; i++)
		{
			long scaled = (long)(m_audioPcm[i] * gain);
			if (scaled >  32767) scaled =  32767;	//Cannot fire given the cap above; cheap insurance.
			if (scaled < -32768) scaled = -32768;
			m_audioPcm[i] = (short)scaled;
		}
	}

	if (getenv("MC2_VIDEO_CENSUS"))
	{
		fprintf(stderr, "[movie] '%s' audio gain: asked %.2fx, %.2fx in samples "
		                "(peak %ld -> %ld), %.2fx left for the channel\n",
		        m_MC2Name ? m_MC2Name : "?", m_audioGain, gain,
		        peak, (long)(peak * gain), m_residualGain);
		fflush(stderr);
	}
}

//-----------------------------------------------------------------------
// PORT (M8-B4): hand the decoded track to STREAM3, from update() rather than
// init(), and KEEP TRYING until it is taken.
//
// Two things forced this apart from init. First, a movie can be created and
// immediately stopped -- MissionSelectionScreen does exactly that, twice, and
// only starts it when the player presses play -- so starting the sound at init
// means starting it for a movie that is not playing, and losing it. Second,
// SoundSystem::playDigitalMemory refuses while STREAM3 still has a sound on it
// (`stream3Active`), and a single attempt at init turns that momentary refusal
// into permanent silence for the whole movie. Retrying costs one boolean test
// per frame and the channel frees itself.
void MC2Movie::tryStartMovieAudio (void)
{
	if (!m_audioPcm || m_audioStarted || !soundSystem)
		return;

	const long result = soundSystem->playDigitalMemory(
							m_audioPcm, m_audioSamples * (long)sizeof(short),
							m_decoder ? m_decoder->AudioChannels() : 0,
							m_decoder ? m_decoder->AudioSampleRate() : 0,
							m_residualGain);

	// PORT (M8-B35): playDigitalMemory now returns the STREAM3 token, so
	// success is a POSITIVE value, not NO_ERR. Testing == NO_ERR here after
	// that change would read every successful start as a refusal.
	if (result > 0)
	{
		m_audioStarted = true;
		m_streamToken = (unsigned long)result;
		if (getenv("MC2_VIDEO_CENSUS"))
		{
			fprintf(stderr, "[movie] '%s' audio started on STREAM3 after %ld refusal(s)\n",
			        m_MC2Name ? m_MC2Name : "?", m_audioRefusals);
			fflush(stderr);
		}
		return;
	}

	// Refused. Report the FIRST one and then every 60th, so a channel that
	// never frees leaves a visible trail without filling the log. Without this
	// the census could not tell "never tried" apart from "tried and refused",
	// and those have completely different causes.
	if (getenv("MC2_VIDEO_CENSUS") && (m_audioRefusals == 0 || (m_audioRefusals % 60) == 0))
	{
		fprintf(stderr, "[movie] '%s' STREAM3 refused the audio (attempt %ld, rc=%ld)\n",
		        m_MC2Name ? m_MC2Name : "?", m_audioRefusals + 1, result);
		fflush(stderr);
	}
	m_audioRefusals++;
}

//-----------------------------------------------------------------------
// Stop the channel BEFORE freeing the buffer it is reading from -- the resource
// holds this pointer, not a copy of it.
void MC2Movie::releaseMovieAudio (void)
{
	if (!m_audioPcm)
		return;

	// Only if THIS movie still owns the channel. Stopping unconditionally would
	// silence whatever took STREAM3 after us -- an objective's scripted WAV, or
	// the next movie in a sequence.
	//
	// PORT (M8-B35): "started it" is no longer enough -- something may have
	// taken the channel since. The token settles it inside stopSupportSample.
	if (m_audioStarted && m_streamToken && soundSystem)
		soundSystem->stopSupportSample("MC2Movie", m_streamToken);
	m_audioStarted = false;
	m_streamToken = 0;

	delete [] m_audioPcm;
	m_audioPcm = NULL;
	m_audioSamples = 0;
}

//-----------------------------------------------------------------------
//Changes rect.  If resize, calls malloc which will be QUITE painful during a MC2 playback
// If just move, its awfully fast.
void MC2Movie::setRect (RECT vRect)
{
	if (((vRect.right - vRect.left) != (MC2Rect.right - MC2Rect.left)) ||
		((vRect.bottom - vRect.top) != (MC2Rect.bottom - MC2Rect.top)))
	{
		//Size changed.  STOP for now to tell people this is bad!
		// May be impossible to do when MC2 is running because MC2 counts on previous frame's contents not changing
		STOP(("Tried to change MC2 Movie Rect size to different one from starting value"));
	}
	else
	{
		//Otherwise, just update the MC2Rect.
		MC2Rect = vRect;
	}
}

//-----------------------------------------------------------------------
//Handles tickling MC2 to make sure we keep playing back
bool MC2Movie::update (void)
{
	// The sidecar wave starts BEFORE the not-playing test, which is where
	// retail started it: a build with no decoder has stillPlaying false from
	// the first tick, and testing first is how the soundtrack went missing.
	if (separateWAVE && !soundStarted)
	{
		soundStarted = true;

		// PORT (M8-B35): keep whatever claim this actually bought. It is 0 when
		// there is no such wave on disk, which is the ordinary case: the sidecar
		// is armed for every movie whose caller passes useWaveFile, including
		// the 109 retail movies that are silent by design and never had one.
		const long token = soundSystem->playDigitalStream(waveName);
		m_streamToken = (token > 0) ? (unsigned long)token : 0;
	}

	if (!stillPlaying || !m_decoder)
		return true;

	if (forceStop)
	{
		stillPlaying = false;
		stopMovieSound();
		return true;
	}

	// Only once the movie is genuinely running: a movie stopped the instant it
	// was created must not have handed its sound to a channel at init.
	tryStartMovieAudio();

	if (m_paused)
		return false;

	const double elapsed = gos_GetElapsedTime(1) - m_startTime;
	if (!m_decoder->Advance(elapsed))
	{
		// PORT (M8-B6): the picture is finished, but DO NOT report done while
		// this movie's own soundtrack is still running. Saying done gets the
		// object deleted by the caller, which frees the very buffer
		// gosAudio_UserMemory is reading from, so the last of the line is lost.
		//
		// Measured 2026-08-16: every comms movie's audio outlasts its video by
		// 60-90 ms (mc2_04c 22.133 v / 22.194 a) and the census confirmed the
		// channel was still in PlayOnce at every stop -- "STILL PLAYING -- being
		// cut". Across all 76 movies with a track the worst case is under 0.5 s.
		//
		// Bounded by the track's own length rather than by asking the channel,
		// so a channel taken over by another sound cannot keep this movie alive
		// forever.
		if (audioStillOwed(elapsed))
			return false;

		stillPlaying = false;
		stopMovieSound();
		return true;
	}

	// Only when the picture actually changed. The movies are 15 fps against a
	// 60 Hz caller, so uploading every tick would lock, copy and unlock every
	// tile four times over for the same frame.
	if (m_decoder->FrameIsNew())
		BltMovieFrame();

	return false;
}

//-----------------------------------------------------------------------
// True while this movie's own decoded track has time left to run. Computed from
// the sample count, so it terminates whatever the channel is doing.
bool MC2Movie::audioStillOwed (double elapsed)
{
	if (!m_audioStarted || !m_audioPcm || m_audioSamples <= 0 || !m_decoder)
		return false;

	const long channels = m_decoder->AudioChannels() > 0 ? m_decoder->AudioChannels() : 1;
	const long rate     = m_decoder->AudioSampleRate() > 0 ? m_decoder->AudioSampleRate() : 48000;
	const double seconds = (double)(m_audioSamples / channels) / (double)rate;

	return elapsed < seconds;
}

//-----------------------------------------------------------------------
void MC2Movie::stopMovieSound (void)
{
	// PORT (M8-B6): is this cutting audio that had not finished? The owner
	// reports Renard's voice stopping while his video runs on. PlayOnce here
	// means the channel was still going when the video ended, which would make
	// this call the culprit; Stop means the sound had already finished on its
	// own and the truncation is somewhere else entirely.
	// PORT (M8-B28): THE MODE ALONE CANNOT ANSWER THIS, AND IT WAS READ AS IF
	// IT COULD. A 2026-08-26 playtest logged ten movie tracks started, ZERO
	// finished on their own, and seven cut here with the mode reading
	// PlayOnce. That looks damning and proves nothing: PlayOnce is also the
	// mode one millisecond before the last sample drains, so "cut with three
	// seconds left" and "cut at the boundary" are indistinguishable from it.
	//
	// M8-B6 already holds the movie open while audioStillOwed(elapsed), so the
	// expected shortfall is <= 0. Print the number instead of the adjective:
	// audioSeconds is the track's true length, elapsed is how long it has been
	// running, and shortfall is what was thrown away. A POSITIVE shortfall is
	// the owner's missing dialogue with a size on it; <= 0 exonerates this call
	// site, which is the outcome this probe is designed to be able to return.
	if (m_audioStarted && getenv("MC2_VIDEO_CENSUS"))
	{
		const gosAudio_PlayMode mode = gosAudio_GetChannelPlayMode(STREAM3CHANNEL);
		const long chans = (m_decoder && m_decoder->AudioChannels() > 0) ?
		                   m_decoder->AudioChannels() : 1;
		const long rate  = (m_decoder && m_decoder->AudioSampleRate() > 0) ?
		                   m_decoder->AudioSampleRate() : 48000;
		const double audioSeconds = (m_audioSamples > 0) ?
		                   (double)(m_audioSamples / chans) / (double)rate : 0.0;
		const double elapsed = gos_GetElapsedTime(1) - m_startTime;
		fprintf(stderr, "[movie] '%s' stopping sound; STREAM3 mode=%d (%s) "
		                "audioSeconds=%.3f elapsed=%.3f shortfall=%.3f\n",
		        m_MC2Name ? m_MC2Name : "?", (int)mode,
		        mode == gosAudio_PlayOnce ? "channel still in PlayOnce"
		                                  : "already finished",
		        audioSeconds, elapsed, audioSeconds - elapsed);
		fflush(stderr);
	}

	// PORT (M8-B35): THIS LINE WAS THE DEFECT. It used to read
	// `if ((separateWAVE || m_audioStarted) && soundSystem)` and then stop
	// STREAM3 with no token at all, which silenced the channel whether this
	// movie had ever held it or not. separateWAVE stays TRUE for a movie with
	// no track of its own -- prepareMovieAudio declines at "file carries no
	// track" BEFORE the block that would clear it -- so a silent movie whose
	// sidecar wave does not exist tore down a voice line it never touched.
	//
	// MEASURED, in the owner's own session log of 2026-09-04, three lines in
	// order (bug-shots\playtest-20260904-085735.err.log):
	//     25406  no wave file for scripted stream 'meat1'
	//     25517  STREAM3 start scripted mc2_15_4
	//     25715  STREAM3 STOPPED by MC2Movie
	// mc2_15_4 is mission 15's "Rebel Blow Up Sensor" line, 4.83 s of audio,
	// cut about halfway. The culprit is the PILOT VIDEO 'meat1' -- line 25406
	// is the only "no wave file" line in the whole session and it is that movie
	// arming its sidecar. A movie that legitimately owned the channel would
	// have logged "STREAM3 start movie track"; the only two in that session are
	// 'msft' and 'choice_3', both long finished. So the stopper held nothing.
	//
	// This is NOT rare. 109 of the 185 retail movies declare zero audio tracks
	// and none of them has a sidecar wave, and the pilot chatter videos are the
	// ones a player sees constantly -- so any pilot video finishing while a
	// scripted line was talking used to cut it off.
	//
	// The M8-B28 ownership guard existed for this and had never fired for
	// anyone: it is `if (token && token != stream3Token)`, token defaults to 0,
	// and getStream3Token() had zero callers in the whole tree. Passing the
	// token is what finally arms it.
	if (m_streamToken && soundSystem)
		soundSystem->stopSupportSample("MC2Movie", m_streamToken);
	else if (separateWAVE && getenv("MC2_VIDEO_CENSUS"))
	{
		// The case that used to cut the voice off. Printing it is also how a
		// build is proved to contain this fix: the literal below exists in no
		// earlier binary.
		fprintf(stderr, "[movie] '%s' M8-B35: held no STREAM3 claim, left it alone\n",
		        m_MC2Name ? m_MC2Name : "?");
		fflush(stderr);
	}

	// Both paths become re-armable, so a later restart() plays them again
	// instead of silently believing they are already going.
	soundStarted = false;
	m_audioStarted = false;
	m_streamToken = 0;
}

//-----------------------------------------------------------------------
// PORT (M8-B4): see the header. The clock is frozen rather than the caller
// being trusted to stop calling update() -- MissionSelectionScreen calls it
// every frame regardless.
void MC2Movie::pause (bool pauseState)
{
	if (pauseState && !m_paused)
	{
		m_pausedAt = gos_GetElapsedTime(1) - m_startTime;
		m_paused = true;
	}
	else if (!pauseState && m_paused)
	{
		m_startTime = gos_GetElapsedTime(1) - m_pausedAt;
		m_paused = false;
	}

	forceStop = false;
}

//-----------------------------------------------------------------------
// PORT (M8-B4): a real restart -- clock, decoder and sound.
void MC2Movie::restart (void)
{
	stopMovieSound();

	if (m_decoder)
		m_decoder->Rewind();

	stillPlaying = true;
	forceStop = false;
	m_paused = false;
	m_pausedAt = 0.0;
	m_startTime = gos_GetElapsedTime(1);

	// stopMovieSound() has already cleared soundStarted and m_audioStarted, so
	// update() re-arms whichever path this movie uses.
	if (getenv("MC2_VIDEO_CENSUS"))
	{
		fprintf(stderr, "[movie] '%s' restarted (wave=%d ownTrack=%ld samples)\n",
		        m_MC2Name ? m_MC2Name : "?", separateWAVE ? 1 : 0, m_audioSamples);
		fflush(stderr);
	}
}

//-----------------------------------------------------------------------
//Actually moves frame data from MC2 to surface and/or texture(s) 
void MC2Movie::BltMovieFrame (void)
{
	if (!m_decoder || !m_texturesReady)
		return;

	const unsigned char* frame = m_decoder->Bgra();
	if (!frame)
		return;

	// The decoder hands back B,G,R,A, which is the byte order gos_LockTexture
	// exposes through its unsigned long*, so this is a straight row copy per
	// tile rather than a per-pixel swizzle.
	for (DWORD tile = 0; tile < totalTexturesUsed; tile++)
	{
		const DWORD tileX = (tile % numWide) * singleTextureSize;
		const DWORD tileY = (tile / numWide) * singleTextureSize;

		TEXTUREPTR textureData;
		gos_LockTexture(mc2TextureNodeIndex[tile],0,0,&textureData);

		for (DWORD y = 0; y < singleTextureSize; y++)
		{
			const DWORD sourceY = tileY + y;
			unsigned long* destination = textureData.pTexture + y * textureData.Width;
			if (sourceY >= (DWORD)m_frameHeight)
			{
				memset(destination,0,textureData.Width * sizeof(unsigned long));
				continue;
			}

			DWORD copyWidth = singleTextureSize;
			if (tileX + copyWidth > (DWORD)m_frameWidth)
				copyWidth = (DWORD)m_frameWidth - tileX;

			memcpy(destination,
				   frame + ((size_t)sourceY * m_frameWidth + tileX) * 4,
				   copyWidth * sizeof(unsigned long));
			if (copyWidth < textureData.Width)
				memset(destination + copyWidth,0,
					   (textureData.Width - copyWidth) * sizeof(unsigned long));
		}

		gos_UnLockTexture(mc2TextureNodeIndex[tile]);
	}
}

//-----------------------------------------------------------------------
//Actually draws the MC2 texture using gos_DrawTriangle.
void MC2Movie::render (void)
{
	// PORT (M8-B4): the mission-select video is AUDIBLE BUT INVISIBLE. The old
	// "first quad" census is a one-shot static, so it reports the first movie of
	// the session and nothing else -- it could not see this at all. Report per
	// movie instead, once, saying either why nothing was drawn or what was.
	const bool census = getenv("MC2_VIDEO_CENSUS") != NULL;
	if (!stillPlaying || !m_texturesReady || !m_frameWidth || !m_frameHeight)
	{
		if (census && !m_reportedRender)
		{
			m_reportedRender = true;
			fprintf(stderr, "[movie] '%s' NOT drawn: playing=%d textures=%d frame=%ldx%ld\n",
			        m_MC2Name ? m_MC2Name : "?", stillPlaying ? 1 : 0,
			        m_texturesReady ? 1 : 0, m_frameWidth, m_frameHeight);
			fflush(stderr);
		}
		return;
	}

	// Same 2D textured-quad idiom the rest of the game uses (gameTacMap.cpp).
	const float left   = (float)MC2Rect.left;
	const float top    = (float)MC2Rect.top;
	const float width  = (float)(MC2Rect.right - MC2Rect.left);
	const float height = (float)(MC2Rect.bottom - MC2Rect.top);
	if (width <= 0.0f || height <= 0.0f)
	{
		if (census && !m_reportedRender)
		{
			m_reportedRender = true;
			fprintf(stderr, "[movie] '%s' NOT drawn: empty rect %ldx%ld\n",
			        m_MC2Name ? m_MC2Name : "?", (long)width, (long)height);
			fflush(stderr);
		}
		return;
	}

	if (census && !m_reportedRender)
	{
		m_reportedRender = true;
		fprintf(stderr, "[movie] '%s' DRAWN: %ld tiles at x %.1f..%.1f y %.1f..%.1f (frame %ldx%ld)\n",
		        m_MC2Name ? m_MC2Name : "?", (long)totalTexturesUsed,
		        left, left + width, top, top + height, m_frameWidth, m_frameHeight);
		fflush(stderr);
	}

	gos_SetRenderState(gos_State_AlphaMode, gos_Alpha_OneZero);
	gos_SetRenderState(gos_State_Specular, FALSE);
	gos_SetRenderState(gos_State_AlphaTest, FALSE);
	gos_SetRenderState(gos_State_Filter, gos_FilterBiLinear);
	gos_SetRenderState(gos_State_ZWrite, 0);
	gos_SetRenderState(gos_State_ZCompare, 0);

	for (DWORD tile = 0; tile < totalTexturesUsed; tile++)
	{
		const DWORD tileX = (tile % numWide) * singleTextureSize;
		const DWORD tileY = (tile / numWide) * singleTextureSize;

		DWORD tileW = singleTextureSize;
		if (tileX + tileW > (DWORD)m_frameWidth)  tileW = (DWORD)m_frameWidth  - tileX;
		DWORD tileH = singleTextureSize;
		if (tileY + tileH > (DWORD)m_frameHeight) tileH = (DWORD)m_frameHeight - tileY;

		const float x0 = left + width  * ((float)tileX / (float)m_frameWidth);
		const float x1 = left + width  * ((float)(tileX + tileW) / (float)m_frameWidth);
		const float y0 = top  + height * ((float)tileY / (float)m_frameHeight);
		const float y1 = top  + height * ((float)(tileY + tileH) / (float)m_frameHeight);

		// The tile texture is padded to a power of two, so the used part is
		// only the first tileW/tileH texels.
		const float u1 = (float)tileW / (float)singleTextureSize;
		const float v1 = (float)tileH / (float)singleTextureSize;

		// PORT (M8-B1): the corners actually submitted, once, so where the quad
		// LANDS can be compared against where the rect SAID.
		static bool reportedQuad = false;
		if (!reportedQuad && getenv("MC2_VIDEO_CENSUS"))
		{
			reportedQuad = true;
			fprintf(stderr, "[movie] first quad x %.1f..%.1f y %.1f..%.1f u0..%.3f v0..%.3f\n",
			        x0, x1, y0, y1, u1, v1);
			fflush(stderr);
		}

		gos_SetRenderState(gos_State_Texture, mc2TextureNodeIndex[tile]);

		gos_VERTEX corners[4];
		for (long i = 0; i < 4; i++)
		{
			corners[i].rhw  = 1.0f;
			corners[i].argb = 0xffffffff;
			corners[i].frgb = 0;
			corners[i].z    = 0.0f;
		}
		corners[0].x = x0; corners[0].y = y0; corners[0].u = 0.0f; corners[0].v = 0.0f;
		corners[1].x = x1; corners[1].y = y0; corners[1].u = u1;   corners[1].v = 0.0f;
		corners[2].x = x0; corners[2].y = y1; corners[2].u = 0.0f; corners[2].v = v1;
		corners[3].x = x1; corners[3].y = y1; corners[3].u = u1;   corners[3].v = v1;

		gos_VERTEX triangle[3];
		triangle[0] = corners[0]; triangle[1] = corners[1]; triangle[2] = corners[2];
		gos_DrawTriangles(triangle,3);
		triangle[0] = corners[1]; triangle[1] = corners[3]; triangle[2] = corners[2];
		gos_DrawTriangles(triangle,3);
	}
}

//--//-----------------------------------------------------------------------
// PORT (M8-1): frees the decoder, the frame textures and the soundtrack. Safe
// to call twice -- the destructor and a failed init both reach it.
void MC2Movie::releaseDecoder (void)
{
	releaseMovieAudio();

	if (m_decoder)
	{
		m_decoder->Close();
		delete m_decoder;
		m_decoder = NULL;
	}

	if (m_texturesReady)
	{
		for (DWORD i = 0; i < totalTexturesUsed; i++)
		{
			if (mc2TextureNodeIndex[i] != 0xffffffff)
			{
				gos_DestroyTexture(mc2TextureNodeIndex[i]);
				mc2TextureNodeIndex[i] = 0xffffffff;
			}
		}
		m_texturesReady = false;
	}

	stillPlaying = false;
}

