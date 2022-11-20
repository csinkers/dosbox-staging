/*
 *  Copyright (C) 2022 Jon Dennis
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

//
// This file contains the reelmagic MPEG player code...
//

#include "reelmagic.h"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "dos_system.h"
#include "logging.h"
#include "mixer.h"
#include "setup.h"

// bring in the MPEG-1 decoder library...
#define PL_MPEG_IMPLEMENTATION
#include "mpeg_decoder.h"

// global config
static ReelMagic_PlayerConfiguration _globalDefaultPlayerConfiguration;
static float _audioLevel                 = 1.5f;
static Bitu _audioFifoSize               = 20;
static Bitu _audioFifoDispose            = 5;

constexpr unsigned int common_magic_key   = 0x40044041;
constexpr unsigned int thehorde_magic_key = 0xC39D7088;

static unsigned int _initialMagicKey = common_magic_key;
static unsigned int _magicalFcodeOverride = 0; // 0 = no override

//
// Internal class utilities...
//
namespace {
// XXX currently duplicating this in realmagic_*.cpp files to avoid header pollution... TDB if this
// is a good idea...
struct RMException : ::std::exception {
	std::string _msg = {};
	RMException(const char* fmt = "General ReelMagic Exception", ...)
	{
		va_list vl;
		va_start(vl, fmt);
		_msg.resize(vsnprintf(&_msg[0], 0, fmt, vl) + 1);
		va_end(vl);
		va_start(vl, fmt);
		vsnprintf(&_msg[0], _msg.size(), fmt, vl);
		va_end(vl);
		LOG(LOG_REELMAGIC, LOG_ERROR)("%s", _msg.c_str());
	}
	virtual ~RMException() throw() {}
	virtual const char* what() const throw()
	{
		return _msg.c_str();
	}
};

#define ARRAY_COUNT(T) (sizeof(T) / sizeof(T[0]))
class AudioSampleFIFO {
	struct Frame {
		bool produced        = false;
		Bitu samplesConsumed = 0;
		struct {
			int16_t left  = 0;
			int16_t right = 0;
		} samples[PLM_AUDIO_SAMPLES_PER_FRAME] = {};
		inline Frame() : produced(false) {}
	};
	Frame _fifo[100]; // up to 100 is roughly 512k of RAM
	const Bitu _fifoMax;
	const Bitu _disposeFrameCount;
	Bitu _producePtr;
	Bitu _consumePtr;
	Bitu _sampleRate;

	inline void DisposeForProduction()
	{
		LOG(LOG_REELMAGIC, LOG_WARN)
		("Audio FIFO consumer not keeping up. Disposing %u Interleaved Samples",
		 (unsigned)(_disposeFrameCount * ARRAY_COUNT(_fifo[0].samples)));
		for (Bitu i = 0; i < _disposeFrameCount; ++i) {
			_fifo[_consumePtr++].produced = false;
			if (_consumePtr >= _fifoMax)
				_consumePtr = 0;
		}
	}

	inline int16_t ConvertSample(const float samp)
	{
		return static_cast<int16_t>(samp * 32767.0f * _audioLevel);
	}

	static Bitu ComputeFifoMax(const Bitu fifoMax)
	{
		Bitu rv = _audioFifoSize;
		if (rv > fifoMax) {
			rv = fifoMax;
			LOG(LOG_REELMAGIC, LOG_WARN)
			("Requested audio FIFO size %u is too big. Limiting to %u",
			 (unsigned)_audioFifoSize,
			 (unsigned)rv);
		}
		return rv;
	}

	static Bitu ComputeDisposeFrameCount(const Bitu fifoSize)
	{
		Bitu rv = _audioFifoDispose;
		if (rv > fifoSize) {
			rv = fifoSize;
			LOG(LOG_REELMAGIC, LOG_WARN)
			("Requested audio FIFO dispose frame count %u is too big. Limiting to %u",
			 (unsigned)_audioFifoDispose,
			 (unsigned)rv);
		}
		return rv;
	}

public:
	AudioSampleFIFO()
	        : _fifoMax(ComputeFifoMax(ARRAY_COUNT(_fifo))),
	          _disposeFrameCount(ComputeDisposeFrameCount(_fifoMax)),
	          _producePtr(0),
	          _consumePtr(0),
	          _sampleRate(0)
	{}
	inline Bitu GetSampleRate() const
	{
		return _sampleRate;
	}
	inline void SetSampleRate(const Bitu value)
	{
		_sampleRate = value;
	}

	// consumer -- 1 sample include left and right
	inline Bitu SamplesAvailableForConsumption()
	{
		const Frame& f = _fifo[_consumePtr];
		if (!f.produced)
			return 0;
		return ARRAY_COUNT(f.samples) - f.samplesConsumed;
	}
	inline const int16_t* GetConsumableInterleavedSamples()
	{
		const Frame& f = _fifo[_consumePtr];
		return &f.samples[f.samplesConsumed].left;
	}
	inline void Consume(const Bitu sampleCount)
	{
		Frame& f = _fifo[_consumePtr];
		f.samplesConsumed += sampleCount;
		if (f.samplesConsumed >= ARRAY_COUNT(f.samples)) {
			f.produced = false;
			if (++_consumePtr >= _fifoMax)
				_consumePtr = 0;
		}
	}

	// producer...
	inline void Produce(const plm_samples_t& s)
	{
		Frame& f = _fifo[_producePtr];
		if (f.produced)
			DisposeForProduction(); // WARNING dropping samples !?

		for (Bitu i = 0; i < ARRAY_COUNT(s.interleaved); i += 2) {
			f.samples[i >> 1].left  = ConvertSample(s.interleaved[i]);
			f.samples[i >> 1].right = ConvertSample(s.interleaved[i + 1]);
		}

		f.samplesConsumed = 0;
		f.produced        = true;
		if (++_producePtr >= _fifoMax)
			_producePtr = 0;
	}
	inline void Clear()
	{
		for (Bitu i = 0; i < _fifoMax; ++i) {
			_fifo[i].produced = false;
		}
		_producePtr = 0;
		_consumePtr = 0;
	}
};
} // namespace

static void ActivatePlayerAudioFifo(AudioSampleFIFO& fifo);
static void DeactivatePlayerAudioFifo(AudioSampleFIFO& fifo);

//
// implementation of a "ReelMagic Media Player" and handles begins here...
//
namespace {
class ReelMagic_MediaPlayerImplementation : public ReelMagic_MediaPlayer,
                                            public ReelMagic_VideoMixerMPEGProvider {
	// creation parameters...
	ReelMagic_MediaPlayerFile* const _file = {};
	ReelMagic_PlayerConfiguration _config  = {};
	ReelMagic_PlayerAttributes _attrs      = {};

	// running / adjustable variables...
	bool _stopOnComplete = {};
	bool _playing        = {};

	// output state...
	float _vgaFps                          = {};
	float _vgaFramesPerMpegFrame           = {};
	float _waitVgaFramesUntilNextMpegFrame = {};
	bool _drawNextFrame                    = {};

	// stuff about the MPEG decoder...
	plm_t* _plm                   = {};
	plm_frame_t* _nextFrame       = {};
	float _framerate              = {};
	uint8_t _magicalRSizeOverride = {};

	AudioSampleFIFO _audioFifo = {};

	static void plmBufferLoadCallback(plm_buffer_t* self, void* user)
	{
		// note: based on plm_buffer_load_file_callback()
		try {
			if (self->discard_read_bytes) {
				plm_buffer_discard_read_bytes(self);
			}
			auto bytes_available = self->capacity - self->length;
			if (bytes_available > 4096)
				bytes_available = 4096;
			const uint32_t bytes_read = ((ReelMagic_MediaPlayerImplementation*)user)
			                                    ->_file->Read(self->bytes + self->length,
			                                                  static_cast<uint16_t>(
			                                                          bytes_available));
			self->length += bytes_read;

			if (bytes_read == 0) {
				self->has_ended = TRUE;
			}
		} catch (...) {
			self->has_ended = TRUE;
		}
	}
	static void plmBufferSeekCallback([[maybe_unused]] plm_buffer_t* self, void* user, size_t absPos)
	{
		assert(absPos <= UINT32_MAX);
		try {
			((ReelMagic_MediaPlayerImplementation*)user)
			        ->_file->Seek(static_cast<uint32_t>(absPos), DOS_SEEK_SET);
		} catch (...) {
			// XXX what to do on failure !?
		}
	}

	static void plmDecodeMagicalPictureHeaderCallback(plm_video_t* self, void* user)
	{
		switch (self->picture_type) {
		case PLM_VIDEO_PICTURE_TYPE_B:
			self->motion_backward.r_size =
			        ((ReelMagic_MediaPlayerImplementation*)user)->_magicalRSizeOverride;
			// fallthrough
		case PLM_VIDEO_PICTURE_TYPE_PREDICTIVE:
			self->motion_forward.r_size = ((ReelMagic_MediaPlayerImplementation*)user)->_magicalRSizeOverride;
		}
	}

	void advanceNextFrame()
	{
		_nextFrame = plm_decode_video(_plm);
		if (_nextFrame == NULL) {
			// note: will return NULL frame once when looping... give it one more go...
			if (plm_get_loop(_plm))
				_nextFrame = plm_decode_video(_plm);
			if (_nextFrame == NULL)
				_playing = false;
		}
	}

	void decodeBufferedAudio()
	{
		if (!_plm->audio_decoder)
			return;
		plm_samples_t* samples;
		while (plm_buffer_get_remaining(_plm->audio_decoder->buffer) > 0) {
			samples = plm_audio_decode(_plm->audio_decoder);
			if (samples == NULL)
				break;
			_audioFifo.Produce(*samples);
		}
	}

	unsigned FindMagicalFCode()
	{
		// now this is some mighty fine half assery...
		// i'm sure this is suppoed to be done on a per-picture basis, but for now, this
		// hack seems to work ok... the idea here is that MPEG-1 assets with a picture_rate
		// code >= 0x9 in the MPEG sequence header have screwed up f_code values. i'm not
		// sure why but this may be some form of copy and/or clone protection for ReelMagic.
		// pictures with a temporal sequence number of either 3 or 8 seem to contain a
		// truthful f_code when a "key" of 0x40044041 (ReelMagic default) is given to us and
		// a temporal sequence number of 4 seems to contain the truthful f_code when a "key"
		// of 0xC39D7088 is given to us
		//
		// for now, this hack scrubs the MPEG file in search of the first P or B pictures
		// with a temporal sequence number matching a truthful value based on the player's
		// "magic key" the player then applies the found f_code value as a global static
		// forward and backward value for this entire asset.
		//
		// ultimately, this should probably be done on a per-picture basis using some sort
		// of algorithm to translate the screwed-up values on-the-fly...

		unsigned result = 0;

		const int audio_enabled = plm_get_audio_enabled(_plm);
		const int loop_enabled  = plm_get_loop(_plm);
		plm_rewind(_plm);
		plm_set_audio_enabled(_plm, FALSE);
		plm_set_loop(_plm, FALSE);

		do {
			if (plm_buffer_find_start_code(_plm->video_decoder->buffer,
			                               PLM_START_PICTURE) == -1) {
				break;
			}
			const unsigned temporal_seqnum = plm_buffer_read(_plm->video_decoder->buffer, 10);
			const unsigned picture_type = plm_buffer_read(_plm->video_decoder->buffer, 3);
			if ((picture_type == PLM_VIDEO_PICTURE_TYPE_PREDICTIVE) ||
			    (picture_type == PLM_VIDEO_PICTURE_TYPE_B)) {
				plm_buffer_skip(_plm->video_decoder->buffer, 16); // skip vbv_delay
				plm_buffer_skip(_plm->video_decoder->buffer, 1);  // skip full_px
				result = plm_buffer_read(_plm->video_decoder->buffer, 3);
				switch (_config.MagicDecodeKey) {
				case thehorde_magic_key:
					if (temporal_seqnum != 4)
						result = 0;
					break;

				default:
					LOG(LOG_REELMAGIC, LOG_WARN)
					("Unknown magic key: 0x%08X. Defaulting to the common key: 0x%08X",
					 _config.MagicDecodeKey, common_magic_key);
					// fall-through

				// most ReelMagic games seem to use this "key"
				case common_magic_key:
					// tsn=3 and tsn=8 seem to contain truthful
					if ((temporal_seqnum != 3) && (temporal_seqnum != 8))
						result = 0;
					break;
				}
			}
		} while (result == 0);

		plm_set_loop(_plm, loop_enabled);
		plm_set_audio_enabled(_plm, audio_enabled);
		plm_rewind(_plm);

		return result;
	}

	void CollectVideoStats()
	{
		_attrs.PictureSize.Width  = static_cast<uint16_t>(plm_get_width(_plm));
		_attrs.PictureSize.Height = static_cast<uint16_t>(plm_get_height(_plm));
		if (_attrs.PictureSize.Width && _attrs.PictureSize.Height) {
			if (_plm->video_decoder->seqh_picture_rate >= 0x9) {
				LOG(LOG_REELMAGIC, LOG_NORMAL)
				("Detected a magical picture_rate code of 0x%X.",
				 (unsigned)_plm->video_decoder->seqh_picture_rate);
				const unsigned magical_f_code = _magicalFcodeOverride
				                                      ? _magicalFcodeOverride
				                                      : FindMagicalFCode();
				if (magical_f_code) {
					const auto reduced_f_code = magical_f_code - 1;
					assert(reduced_f_code <= UINT8_MAX);
					_magicalRSizeOverride = static_cast<uint8_t>(reduced_f_code);
					plm_video_set_decode_picture_header_callback(
					        _plm->video_decoder,
					        &plmDecodeMagicalPictureHeaderCallback,
					        this);
					LOG(LOG_REELMAGIC, LOG_NORMAL)
					("Applying static %u:%u f_code override",
					 magical_f_code,
					 magical_f_code);
				} else {
					LOG(LOG_REELMAGIC, LOG_WARN)
					("No magical f_code found. Playback will likely be screwed up!");
				}
				_plm->video_decoder->framerate =
				        PLM_VIDEO_PICTURE_RATE[0x7 & _plm->video_decoder->seqh_picture_rate];
			}
			if (_plm->video_decoder->framerate == 0.000) {
				LOG(LOG_REELMAGIC, LOG_ERROR)
				("Detected a bad framerate. Hardcoding to 30. This video will likely not work at all.");
				_plm->video_decoder->framerate = 30.000;
			}
		}
		_framerate = static_cast<float>(plm_get_framerate(_plm));
	}

	void SetupVESOnlyDecode()
	{
		plm_set_audio_enabled(_plm, FALSE);
		if (_plm->audio_decoder) {
			plm_audio_destroy(_plm->audio_decoder);
			_plm->audio_decoder = NULL;
		}
		plm_demux_rewind(_plm->demux);
		_plm->has_decoders      = TRUE;
		_plm->video_packet_type = PLM_DEMUX_PACKET_VIDEO_1;
		if (_plm->video_decoder)
			plm_video_destroy(_plm->video_decoder);
		_plm->video_decoder = plm_video_create_with_buffer(_plm->demux->buffer, FALSE);
	}

public:
	ReelMagic_MediaPlayerImplementation(const ReelMagic_MediaPlayerImplementation&) = delete;
	ReelMagic_MediaPlayerImplementation& operator=(const ReelMagic_MediaPlayerImplementation&) = delete;

	ReelMagic_MediaPlayerImplementation(ReelMagic_MediaPlayerFile* const player_file)
	        : _file(player_file),
	          _stopOnComplete(false),
	          _playing(false),
	          _vgaFps(0.0f),
	          _plm(NULL),
	          _nextFrame(NULL),
	          _magicalRSizeOverride(0)
	{
		memcpy(&_config, &_globalDefaultPlayerConfiguration, sizeof(_config));
		memset(&_attrs, 0, sizeof(_attrs));

		assert(_file);
		auto plmBuf = plm_buffer_create_with_virtual_file(&plmBufferLoadCallback,
		                                                  &plmBufferSeekCallback,
		                                                  this,
		                                                  _file->GetFileSize());
		assert(plmBuf);

		// TRUE means that the buffer is destroyed on failure or when closing _plm
		_plm = plm_create_with_buffer(plmBuf, TRUE);

		if (_plm == NULL) {
			LOG(LOG_REELMAGIC, LOG_ERROR)
			("Player failed creating buffer using file %s",
			 _file->GetFileName());
			plmBuf = NULL;
			return;
		}

		assert(_plm);
		plm_demux_set_stop_on_program_end(_plm->demux, TRUE);

		bool detetectedFileTypeVesOnly = false;
		if (!plm_has_headers(_plm)) {
			// failed to detect an MPEG-1 PS (muxed) stream...
			// try MPEG-ES: assuming video-only...
			detetectedFileTypeVesOnly = true;
			SetupVESOnlyDecode();
		}

		// disable audio buffer load callback so pl_mpeg dont try to "auto fetch" audio
		// samples when we ask it for audio data...
		if (_plm->audio_decoder) {
			_plm->audio_decoder->buffer->load_callback = NULL;
			_audioFifo.SetSampleRate((Bitu)plm_get_samplerate(_plm));
		}

		CollectVideoStats();
		advanceNextFrame(); // attempt to decode the first frame of video...
		if ((_nextFrame == NULL) || (_attrs.PictureSize.Width == 0) ||
		    (_attrs.PictureSize.Height == 0)) {
			// something failed... asset is deemed bad at this point...
			plm_destroy(_plm);
			_plm = NULL;
		}

		if (_plm == NULL) {
			LOG(LOG_REELMAGIC, LOG_ERROR)
			("Failed creating media player: MPEG type-detection failed %s",
			 _file->GetFileName());
		} else {
			LOG(LOG_REELMAGIC, LOG_NORMAL)
			("Created Media Player %s %ux%u @ %0.2ffps %s",
			 detetectedFileTypeVesOnly ? "MPEG-ES" : "MPEG-PS",
			 (unsigned)_attrs.PictureSize.Width,
			 (unsigned)_attrs.PictureSize.Height,
			 (double)_framerate,
			 _file->GetFileName());
			if (_audioFifo.GetSampleRate())
				LOG(LOG_REELMAGIC, LOG_NORMAL)
			("Media Player Audio Decoder Enabled @ %uHz",
			 (unsigned)_audioFifo.GetSampleRate());
		}
	}
	virtual ~ReelMagic_MediaPlayerImplementation()
	{
		LOG(LOG_REELMAGIC, LOG_NORMAL)
		("Destroying Media Player #%u with file %s", GetBaseHandle(), _file->GetFileName());
		DeactivatePlayerAudioFifo(_audioFifo);
		if (ReelMagic_GetVideoMixerMPEGProvider() == this)
			ReelMagic_SetVideoMixerMPEGProvider(NULL);
		if (_plm != NULL)
			plm_destroy(_plm);
		delete _file;
	}

	//
	// ReelMagic_VideoMixerMPEGProvider implementation here...
	//
	void OnVerticalRefresh(void* const outputBuffer, const float fps)
	{
		if (fps != _vgaFps) {
			_vgaFps                = fps;
			_vgaFramesPerMpegFrame = _vgaFps;
			_vgaFramesPerMpegFrame /= _framerate;
			_waitVgaFramesUntilNextMpegFrame = _vgaFramesPerMpegFrame;
			_drawNextFrame                   = true;
		}

		if (_drawNextFrame) {
			if (_nextFrame != NULL)
				plm_frame_to_rgb(_nextFrame,
				                 (uint8_t*)outputBuffer,
				                 _attrs.PictureSize.Width * 3);
			decodeBufferedAudio();
			_drawNextFrame = false;
		}

		if (!_playing) {
			if (_stopOnComplete)
				ReelMagic_SetVideoMixerMPEGProvider(NULL);
			return;
		}

		for (_waitVgaFramesUntilNextMpegFrame -= 1.f; _waitVgaFramesUntilNextMpegFrame < 0.f;
		     _waitVgaFramesUntilNextMpegFrame += _vgaFramesPerMpegFrame) {
			advanceNextFrame();
			_drawNextFrame = true;
		}
	}

	const ReelMagic_PlayerConfiguration& GetConfig() const
	{
		return _config;
	}
	// const ReelMagic_PlayerAttributes& GetAttrs() const -- implemented in the
	// ReelMagic_MediaPlayer functions below

	//
	// ReelMagic_MediaPlayer implementation here...
	//
	ReelMagic_PlayerConfiguration& Config()
	{
		return _config;
	}
	const ReelMagic_PlayerAttributes& GetAttrs() const
	{
		return _attrs;
	}
	bool HasDemux() const
	{
		return _plm && _plm->demux->buffer != _plm->video_decoder->buffer;
	}
	bool HasVideo() const
	{
		return _plm && plm_get_video_enabled(_plm);
	}
	bool HasAudio() const
	{
		return _plm && plm_get_audio_enabled(_plm);
	}
	bool IsPlaying() const
	{
		return _playing;
	}

	// Handle registation functions.
	void RegisterBaseHandle(const reelmagic_handle_t handle)
	{
		assert(handle != reelmagic_invalid_handle);
		_attrs.Handles.Base = handle;
	}

	reelmagic_handle_t GetBaseHandle() const
	{
		assert(_attrs.Handles.Base != reelmagic_invalid_handle);
		return _attrs.Handles.Base;
	}

	// The return value indicates if the handle was registered.
	bool RegisterDemuxHandle(const reelmagic_handle_t handle)
	{
		const auto has_demux = HasDemux();
		_attrs.Handles.Demux = has_demux ? handle : reelmagic_invalid_handle;
		return has_demux;
	}

	bool RegisterVideoHandle(const reelmagic_handle_t handle)
	{
		const auto has_video = HasVideo();
		_attrs.Handles.Video = has_video ? handle : reelmagic_invalid_handle;
		return has_video;
	}

	bool RegisterAudioHandle(const reelmagic_handle_t handle)
	{
		const auto has_audio = HasAudio();
		_attrs.Handles.Audio = has_audio ? handle : reelmagic_invalid_handle;
		return has_audio;
	}

	Bitu GetBytesDecoded() const
	{
		if (_plm == NULL)
			return 0;
		// the "real" ReelMagic setup seems to only return values in multiples of 4k...
		// therfore, we must emulate the same behavior here...
		// rounding up the demux position to align....
		// NOTE: I'm not sure if this should be different for DMA streaming mode!
		const Bitu alignTo = 4096;
		Bitu rv            = plm_buffer_tell(_plm->demux->buffer);
		rv += alignTo - 1;
		rv &= ~(alignTo - 1);
		return rv;
	}

	void Play(const PlayMode playMode)
	{
		if (_plm == NULL)
			return;
		if (_playing)
			return;
		_playing = true;
		plm_set_loop(_plm, (playMode == MPPM_LOOP) ? TRUE : FALSE);
		_stopOnComplete = playMode == MPPM_STOPONCOMPLETE;
		ReelMagic_SetVideoMixerMPEGProvider(this);
		ActivatePlayerAudioFifo(_audioFifo);
		_vgaFps = 0.0f; // force drawing of next frame and timing reset
	}
	void Pause()
	{
		_playing = false;
	}
	void Stop()
	{
		_playing = false;
		if (ReelMagic_GetVideoMixerMPEGProvider() == this)
			ReelMagic_SetVideoMixerMPEGProvider(NULL);
	}
	void SeekToByteOffset(const uint32_t offset)
	{
		plm_rewind(_plm);
		plm_buffer_seek(_plm->demux->buffer, (size_t)offset);
		_audioFifo.Clear();

		// this is a hacky way to force an audio decoder reset...
		if (_plm->audio_decoder)
			// something (hopefully not sample rate) changes between byte seeks in crime
			// patrol...
			_plm->audio_decoder->has_header = FALSE;

		advanceNextFrame();
	}
	void NotifyConfigChange()
	{
		if (ReelMagic_GetVideoMixerMPEGProvider() == this)
			ReelMagic_SetVideoMixerMPEGProvider(this);
	}
};
} // namespace

//
// stuff to manage ReelMagic media/decoder/player handles...
//
using player_t = std::shared_ptr<ReelMagic_MediaPlayerImplementation>;
static std::vector<player_t> player_registry = {};

void deregister_player(const player_t& player)
{
	for (auto& p : player_registry) {
		if (p.get() == player.get()) {
			p = {};
		}
	}
}

// Registers one or more handles for the player's elementary streams.
// Returns the base handle on success or the invalid handle on failure.
static reelmagic_handle_t register_player(const player_t& player)
{
	auto get_available_handle = []() {
		// Walk from the first to (potentially) last valid handle
		auto h = reelmagic_first_handle;
		while (h <= reelmagic_last_handle) {

			// Should we grow the registry to accomodate this handle?
			if (player_registry.size() <= h) {
				player_registry.emplace_back();
				continue;
			}
			// Is this handle available (i.e.: unused) in the registry?
			if (!player_registry[h]) {
				return h;
			}
			// Otherwise step forward to the next handle
			++h;
		}
		LOG_ERR("REELMAGIC: Ran out of handles while registering player");
		throw reelmagic_invalid_handle;
	};
	try {
		// At a minimum, we register the player itself
		auto h = get_available_handle();
		player->RegisterBaseHandle(h);
		player_registry[h] = player;

		// The first stream reuses the player's handle
		if (player->RegisterDemuxHandle(h)) {
			h = get_available_handle();
		}
		if (player->RegisterVideoHandle(h)) {
			player_registry[h] = player;
			h = get_available_handle();
		}
		if (player->RegisterAudioHandle(h)) {
			player_registry[h] = player;
		}
	} catch (reelmagic_handle_t invalid_handle) {
		deregister_player(player);
		return invalid_handle;
	}
	return player->GetBaseHandle();
}

reelmagic_handle_t ReelMagic_NewPlayer(struct ReelMagic_MediaPlayerFile* const playerFile)
{
	// so why all this mickey-mouse for simply allocating a handle?
	// the real setup allocates one handle per decoder resource
	// for example, if an MPEG file is opened that only contains a video ES,
	// then only one handle is allocated
	// however, if an MPEG PS file is openened that contains both A/V ES streams,
	// then three handles are allocated. One for system, one for audio, one for video
	//
	// to ensure maximum compatibility, we must also emulate this behavior

	auto player = std::make_shared<ReelMagic_MediaPlayerImplementation>(playerFile);
	return register_player(player);
}

void ReelMagic_DeletePlayer(const reelmagic_handle_t handle)
{
	if (handle < player_registry.size()) {
		if (const auto p = player_registry[handle]; p) {
			deregister_player(p);
		}
	}
}

ReelMagic_MediaPlayer& ReelMagic_HandleToMediaPlayer(const reelmagic_handle_t handle)
{
	if (handle >= player_registry.size() || !player_registry.at(handle)) {
		throw RMException("Invalid handle #%u", handle);
	}

	return *player_registry[handle];
}

void ReelMagic_DeleteAllPlayers()
{
	player_registry = {};
}

//
// audio stuff begins here...
//
mixer_channel_t _rmaudio                                = nullptr;
static AudioSampleFIFO* volatile _activePlayerAudioFifo = NULL;

static void ActivatePlayerAudioFifo(AudioSampleFIFO& fifo)
{
	if (!fifo.GetSampleRate())
		return;
	_activePlayerAudioFifo = &fifo;
	assert(_rmaudio);
	_rmaudio->SetSampleRate(static_cast<int>(_activePlayerAudioFifo->GetSampleRate()));
}

static void DeactivatePlayerAudioFifo(AudioSampleFIFO& fifo)
{
	if (_activePlayerAudioFifo == &fifo)
		_activePlayerAudioFifo = NULL;
}

static AudioFrame _lastAudioSample = {};
static void RMMixerChannelCallback(uint16_t samplesNeeded)
{
	// samplesNeeded is sample count, including both channels...
	if (_activePlayerAudioFifo == NULL) {
		_rmaudio->AddSilence();
		return;
	}
	uint16_t available = 0;
	while (samplesNeeded) {
		available = static_cast<uint16_t>(
		        _activePlayerAudioFifo->SamplesAvailableForConsumption());
		if (available == 0) {
			_rmaudio->AddSamples_sfloat(1, &_lastAudioSample[0]);
			--samplesNeeded;
			continue;
			//      _rmaudio->AddSilence();
			//      return;
		}
		if (samplesNeeded > available) {
			_rmaudio->AddSamples_s16(available,
			                         _activePlayerAudioFifo->GetConsumableInterleavedSamples());
			_lastAudioSample.left =
			        _activePlayerAudioFifo->GetConsumableInterleavedSamples()[available - 2];
			_lastAudioSample.right =
			        _activePlayerAudioFifo->GetConsumableInterleavedSamples()[available - 1];
			_activePlayerAudioFifo->Consume(available);
			samplesNeeded -= available;
		} else {
			_rmaudio->AddSamples_s16(samplesNeeded,
			                         _activePlayerAudioFifo->GetConsumableInterleavedSamples());
			_lastAudioSample.left =
			        _activePlayerAudioFifo->GetConsumableInterleavedSamples()[samplesNeeded - 2];
			_lastAudioSample.right =
			        _activePlayerAudioFifo->GetConsumableInterleavedSamples()[samplesNeeded - 1];
			_activePlayerAudioFifo->Consume(samplesNeeded);
			samplesNeeded = 0;
		}
	}
}

void ReelMagic_EnableAudioChannel(const bool should_enable)
{
	if (should_enable == false) {
		MIXER_RemoveChannel(_rmaudio);
		assert(!_rmaudio);
		return;
	}

	_rmaudio = MIXER_AddChannel(&RMMixerChannelCallback,
	                            use_mixer_rate,
	                            reelmagic_channel_name,
	                            {// ChannelFeature::Sleep,
	                             ChannelFeature::Stereo,
	                             // ChannelFeature::ReverbSend,
	                             // ChannelFeature::ChorusSend,
	                             ChannelFeature::DigitalAudio});
	assert(_rmaudio);
	_rmaudio->Enable(true);
}

static void set_magic_key(const std::string_view key_choice)
{
	if (key_choice == "auto") {
		_initialMagicKey = common_magic_key;
		// default: don't report anything
	} else if (key_choice == "common") {
		_initialMagicKey = common_magic_key;
		LOG_MSG("REELMAGIC: Using the common key: 0x%x", common_magic_key);
	} else if (key_choice == "thehorde") {
		_initialMagicKey = thehorde_magic_key;
		LOG_MSG("REELMAGIC: Using The Horde's key: 0x%x", thehorde_magic_key);
	} else if (unsigned int k; sscanf(key_choice.data(), "%x", &k) == 1) {
		_initialMagicKey = k;
		LOG_MSG("REELMAGIC: Using custom key: 0x%x", k);
	} else {
		LOG_WARNING("REELMAGIC: Failed parsing key choice '%s', using built-in routines",
		            key_choice.data());
		_initialMagicKey = common_magic_key;
	}
}

static void set_fcode(const int fps_code_choice)
{
	// Default
	constexpr auto default_fps_code = 0;

	if (fps_code_choice == default_fps_code) {
		_magicalFcodeOverride = default_fps_code;
		return;
	}

	auto fps_from_code = [=]() {
		switch (fps_code_choice) {
		case 1: return "23.976";
		case 2: return "24";
		case 3: return "25";
		case 4: return "29.97";
		case 5: return "30";
		case 6: return "50";
		case 7: return "59.94";
		};
		return "unknown"; // should never hit this
	};

	// Override with a valid code
	if (fps_code_choice >= 1 && fps_code_choice <= 7) {
		LOG_MSG("REELMAGIC: Overriding the frame rate to %s FPS (code %d)",
		        fps_from_code(),
		        fps_code_choice);
		_magicalFcodeOverride = fps_code_choice;
		return;
	}

	LOG_WARNING("REELMAGIC: Frame rate code '%d' is not between 0 and 7, using built-in routines",
	            fps_code_choice);
	_magicalFcodeOverride = default_fps_code;
}

void ReelMagic_InitPlayer(Section* sec)
{
	assert(sec);
	const auto section = static_cast<Section_prop*>(sec);
	set_magic_key(section->Get_string("reelmagic_key"));

	set_fcode(section->Get_int("reelmagic_fcode"));

	ReelMagic_EnableAudioChannel(true);

	ReelMagic_ResetPlayers();
}

void ReelMagic_ResetPlayers()
{
	ReelMagic_DeleteAllPlayers();

	// set the global configuration default values here...
	ReelMagic_PlayerConfiguration& cfg = _globalDefaultPlayerConfiguration;

	cfg.VideoOutputVisible = true;
	cfg.UnderVga           = false;
	cfg.VgaAlphaIndex      = 0;
	cfg.MagicDecodeKey     = _initialMagicKey;
	cfg.DisplayPosition.X  = 0;
	cfg.DisplayPosition.Y  = 0;
	cfg.DisplaySize.Width  = 0;
	cfg.DisplaySize.Height = 0;
}

ReelMagic_PlayerConfiguration& ReelMagic_GlobalDefaultPlayerConfig()
{
	return _globalDefaultPlayerConfiguration;
}
