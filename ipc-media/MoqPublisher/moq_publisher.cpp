#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <cassert>

#include <moq/rcbuf.h>

#include "moq_publisher.h"

typedef std::lock_guard<std::mutex> Lock;

static const char *VIDEO_TRACK_NAME = "video";
static const char *AUDIO_TRACK_NAME = "audio";
static const char *AUDIO_CODEC = "mp4a.40.2";
static const uint64_t VIDEO_MAX_BITRATE = 4000000;
static const uint64_t AUDIO_MAX_BITRATE = 64000;
static const uint64_t DRAIN_TIMEOUT_US = 1000000;
static const unsigned int STATS_EVERY_POLLS = 10;
/* The MOQtail player (player.moqtail.dev) and relay speak draft 22 only */
static const moq_version_t MOQT_VERSION = MOQ_VERSION_DRAFT_22;
/* The MOQtail player SUBSCRIBEs to the catalog and waits for its next object; the relay
 * forwards only new objects, so a late viewer needs the catalog republished regularly */
static const uint64_t CATALOG_REFRESH_US = 1000000;
static const uint32_t VIDEO_TIMESCALE = 90000;
static const uint32_t VIDEO_DEFAULT_DURATION = VIDEO_TIMESCALE / 15;
static const uint32_t AAC_SAMPLES_PER_FRAME = 1024;
/* The session timeline starts this far before start(), so media captured shortly before
 * the session (audio is ~0.5 s old once AAC-encoded) still lands at a positive time */
static const uint64_t TIMELINE_LEAD_US = 2000000;
static const char *CLOSE_REASONS[] = {"unknown", "clean close", "protocol error", "TLS certificate rejected",
                                      "TLS handshake failed", "transport/timeout"};

static uint64_t nowUs() {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static moq_bytes_t toBytes(const void *data, size_t len) {
	return moq_bytes_t{(const uint8_t*)data, len};
}

static moq_bytes_t toBytes(const std::string &s) {
	return toBytes(s.data(), s.size());
}

static moq_bytes_t toBytes(const char *s) {
	return toBytes(s, strlen(s));
}

/* Namespace parts point into ns, which must outlive them */
static std::vector<moq_bytes_t> splitNamespace(const std::string &ns) {
	std::vector<moq_bytes_t> parts;
	for (size_t begin = 0; begin < ns.size();) {
		size_t slash = std::min(ns.find('/', begin), ns.size());
		if (slash > begin) {
			parts.push_back(toBytes(ns.data() + begin, slash - begin));
		}
		begin = slash + 1;
	}
	return parts;
}

MoqPublisher::MoqPublisher(const std::string &url, const std::string &ns, bool verifyTls, const std::string &caFile)
	: mUrl(url), mNamespace(ns), mCaFile(caFile), mVerifyTls(verifyTls),
	  mOnError([](int, const std::string &message) { fprintf(stderr, "[MOQ] %s\r\n", message.c_str()); }) {}

MoqPublisher::~MoqPublisher() {
	stop();
}

void MoqPublisher::setErrorCallback(const ErrorCallback &callback) {
	assert(callback && !mEndpoint);
	mOnError = callback;
}

void MoqPublisher::setVideoConfig(const VideoConfig &config) {
	assert(!config.record.empty() && !config.codecString.empty() && config.width && config.height);
	Lock lock(mLock);
	mVideoConfig = config;
}

void MoqPublisher::setAudioConfig(const uint8_t asc[2], uint32_t sampleRate, uint32_t numChannels) {
	assert(asc && sampleRate && numChannels);
	Lock lock(mLock);
	mAsc.assign(asc, asc + 2);
	mSampleRate = sampleRate;
	mNumChannels = numChannels;
}

bool MoqPublisher::hasVideoConfig() {
	Lock lock(mLock);
	return !mVideoConfig.record.empty();
}

bool MoqPublisher::hasAudioConfig() {
	Lock lock(mLock);
	return !mAsc.empty();
}

bool MoqPublisher::start() {
	bool ok;
	{
		Lock lock(mLock);
		ok = connect();
	}
	if (!ok) {
		stop(); /* releases whatever connect() got before failing */
		return false;
	}
	/* connect() is asynchronous: the handshake result is logged by isAlive() */
	printf("[MOQ] Connecting %s as %s (%s) ...\r\n", mUrl.c_str(), mNamespace.c_str(), mVideoConfig.codecString.c_str());
	return true;
}

/* Called with mLock held */
bool MoqPublisher::connect() {
	assert(!mEndpoint && !mVideoConfig.record.empty());

	moq_endpoint_cfg_t ec;
	moq_endpoint_cfg_init(&ec);
	ec.url = toBytes(mUrl);
	ec.ca_file = toBytes(mCaFile);
	ec.insecure_skip_verify = !mVerifyTls;
	ec.versions.struct_size = sizeof(ec.versions);
	ec.versions.policy = MOQ_VERSION_POLICY_EXACT;
	ec.versions.versions = &MOQT_VERSION;
	ec.versions.version_count = 1;
	moq_endpoint_t *endpoint = NULL;
	moq_result_t rc = moq_endpoint_connect(&ec, &endpoint);
	if (rc != MOQ_OK) {
		fail(rc, "Connect %s", mUrl.c_str());
		return false;
	}
	mEndpoint = endpoint;

	/* Live preset (DROP_TO_KEYFRAME) never blocks the capture threads; push mode PUBLISHes the tracks.
	 * Without a viewer, media is dropped instead of queued, so a joining player starts at the live edge. */
	std::vector<moq_bytes_t> nsParts = splitNamespace(mNamespace);
	moq_media_sender_cfg_t sc;
	moq_media_sender_cfg_init_live_sized(&sc, sizeof(sc));
	sc.publish_tracks = true;
	sc.drop_without_demand = true;
	sc.catalog_refresh_interval_us = CATALOG_REFRESH_US;
	sc.namespace_.parts = nsParts.data();
	sc.namespace_.count = nsParts.size();
	moq_media_sender_t *sender = NULL;
	rc = moq_media_sender_attach(mEndpoint, &sc, &sender);
	if (rc != MOQ_OK) {
		fail(rc, "Sender attach");
		return false;
	}
	mSender = sender;
	mOriginUs = nowUs() - TIMELINE_LEAD_US;
	mLastVideoPtsUs = 0;
	mLastState = MOQ_ENDPOINT_CONNECTING;
	mAlivePolls = 0;

	moq_cmaf_packager_cfg_t pc;
	moq_cmaf_packager_cfg_init(&pc);
	pc.sample_entry = (mVideoConfig.codec == VIDEO_CODEC_H265) ? MOQ_CMAF_ENTRY_HVC1 : MOQ_CMAF_ENTRY_AVC1;
	pc.codec_config = toBytes(mVideoConfig.record.data(), mVideoConfig.record.size());
	pc.sample_format = MOQ_CMAF_SAMPLE_NAL_LENGTH_PREFIXED;
	pc.timescale = VIDEO_TIMESCALE;
	pc.width = mVideoConfig.width;
	pc.height = mVideoConfig.height;
	moq_media_track_cfg_t tc;
	moq_media_track_cfg_init(&tc);
	tc.media_type = MOQ_MEDIA_TYPE_VIDEO;
	tc.codec = toBytes(mVideoConfig.codecString);
	tc.width = mVideoConfig.width;
	tc.height = mVideoConfig.height;
	tc.bitrate = VIDEO_MAX_BITRATE;
	mVideo.duration = VIDEO_DEFAULT_DURATION;
	if (!openTrack(mVideo, pc, tc, VIDEO_TRACK_NAME)) {
		return false;
	}
	if (mAsc.empty()) {
		return true; /* muted: video only */
	}

	std::string channelConfig = std::to_string(mNumChannels);
	moq_cmaf_packager_cfg_init(&pc);
	pc.sample_entry = MOQ_CMAF_ENTRY_MP4A;
	pc.codec_config = toBytes(mAsc.data(), mAsc.size());
	pc.sample_format = MOQ_CMAF_SAMPLE_AAC_RAW;
	pc.samplerate = mSampleRate;
	pc.channel_count = mNumChannels;
	pc.avg_bitrate = AUDIO_MAX_BITRATE;
	moq_media_track_cfg_init(&tc);
	tc.media_type = MOQ_MEDIA_TYPE_AUDIO;
	tc.codec = toBytes(AUDIO_CODEC);
	tc.samplerate = mSampleRate;
	tc.channel_config = toBytes(channelConfig);
	tc.bitrate = AUDIO_MAX_BITRATE;
	mAudio.duration = AAC_SAMPLES_PER_FRAME;
	return openTrack(mAudio, pc, tc, AUDIO_TRACK_NAME);
}

/* Called with mLock held. Creates the packager, then announces the track with its init segment */
bool MoqPublisher::openTrack(Track &t, const moq_cmaf_packager_cfg_t &pc, moq_media_track_cfg_t &tc, const char *name) {
	t.name = name;
	t.nextDts = 0;
	moq_result_t rc = moq_cmaf_packager_create(NULL, &pc, &t.packager);
	if (rc != MOQ_OK) {
		fail(rc, "%s CMAF packager", name);
		return false;
	}
	tc.name = toBytes(name);
	tc.packaging = MOQ_MEDIA_PACKAGING_CMAF;
	tc.timescale = moq_cmaf_packager_timescale(t.packager);
	tc.init_data = moq_cmaf_packager_init_segment(t.packager);
	tc.is_live = true;
	rc = moq_media_sender_add_track(mSender, &tc, &t.track);
	if (rc != MOQ_OK) {
		fail(rc, "Add %s track", name);
		return false;
	}
	return true;
}

void MoqPublisher::stop() {
	moq_endpoint_t *endpoint;
	moq_media_sender_t *sender;
	{
		Lock lock(mLock);
		endpoint = mEndpoint;
		sender = mSender;
		mEndpoint = nullptr;
		mSender = nullptr;
		for (Track *t : {&mVideo, &mAudio}) {
			moq_cmaf_packager_destroy(t->packager);
			*t = Track();
		}
	}
	if (!endpoint) {
		return;
	}
	/* Drain while the sender is still attached, then child -> stop -> destroy */
	moq_endpoint_drain(endpoint, DRAIN_TIMEOUT_US);
	if (sender) {
		moq_media_sender_destroy(sender);
	}
	moq_endpoint_stop(endpoint);
	moq_endpoint_destroy(endpoint);
}

bool MoqPublisher::isAlive() {
	Lock lock(mLock);
	moq_endpoint_state_t state = mEndpoint ? moq_endpoint_state(mEndpoint) : MOQ_ENDPOINT_CLOSED;
	if (mEndpoint && state != mLastState) {
		logStateChange(state);
		mLastState = state;
	}
	if (state == MOQ_ENDPOINT_ESTABLISHED && ++mAlivePolls % STATS_EVERY_POLLS == 0) {
		logStats();
	}
	return state == MOQ_ENDPOINT_CONNECTING || state == MOQ_ENDPOINT_ESTABLISHED;
}

void MoqPublisher::writeVideo(const uint8_t *data, uint32_t len, bool isKeyFrame, uint64_t ptsUs) {
	Lock lock(mLock);
	if (!mVideo.track) {
		return;
	}
	/* The packager's duration is the previous frame interval: the next frame is not known yet */
	if (mLastVideoPtsUs && ptsUs > mLastVideoPtsUs && ptsUs - mLastVideoPtsUs < 1000000) {
		mVideo.duration = (uint32_t)((ptsUs - mLastVideoPtsUs) * VIDEO_TIMESCALE / 1000000);
	}
	mLastVideoPtsUs = ptsUs;
	Codec::toCmafSample(mVideoConfig.codec, data, len, mVideoSample);
	write(mVideo, mVideoSample.data(), mVideoSample.size(), isKeyFrame, ptsUs);
}

void MoqPublisher::writeAudio(const uint8_t *data, uint32_t len, uint64_t ptsUs) {
	Lock lock(mLock);
	write(mAudio, data, len, true, ptsUs);
}

/* Called with mLock held. Each frame becomes one CMAF chunk (moof+mdat) and one object.
 * A sync object opens a new group: one GOP per group for video, one AAC frame per group
 * for audio (every AAC-LC frame is independently decodable). */
void MoqPublisher::write(Track &t, const uint8_t *data, uint32_t len, bool isSync, uint64_t ptsUs) {
	/* No session (or muted audio), or media captured before the session timeline began */
	if (!t.track || len == 0 || ptsUs < mOriginUs) {
		return;
	}
	/* Capture jitter must not make the decode timeline overlap the previous chunk */
	int64_t dts = (int64_t)std::max((ptsUs - mOriginUs) * moq_cmaf_packager_timescale(t.packager) / 1000000, t.nextDts);
	moq_cmaf_packager_sample_t sample = {toBytes(data, len), dts, dts, t.duration, isSync};
	moq_bytes_t fragment;
	moq_result_t rc = moq_cmaf_packager_write(t.packager, &sample, 1, &fragment);
	if (rc != MOQ_OK) {
		fail(rc, "%s CMAF packaging (%u bytes, sync=%d)", t.name, len, isSync);
		return;
	}
	t.nextDts = dts + t.duration;

	moq_media_send_object_t obj;
	memset(&obj, 0, sizeof(obj));
	obj.struct_size = sizeof(obj);
	rc = moq_rcbuf_create(moq_alloc_default(), fragment.data, fragment.len, &obj.payload);
	if (rc != MOQ_OK) {
		fail(rc, "%s object buffer (%zu bytes)", t.name, fragment.len);
		return;
	}
	obj.is_sync = isSync;
	obj.starts_group = isSync;
	obj.presentation_time_us = ptsUs;
	obj.decode_time_us = ptsUs;
	rc = moq_media_sender_write(mSender, t.track, &obj);
	/* WOULD_BLOCK is the live drop policy (no viewer, or congestion), not a failure */
	if (rc != MOQ_OK && rc != MOQ_ERR_WOULD_BLOCK) {
		fail(rc, "%s write", t.name);
	}
	if (rc != MOQ_OK) {
		moq_rcbuf_decref(obj.payload); /* on any non-OK result the payload is still ours */
	}
}

/* Called with mLock held */
void MoqPublisher::fail(int rc, const char *fmt, ...) {
	char what[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(what, sizeof(what), fmt, args);
	va_end(args);
	mOnError(rc, std::string(what) + " failed: " + moq_strerror(rc) + " (rc=" + std::to_string(rc) + ")");
}

/* Called with mLock held */
void MoqPublisher::logStateChange(moq_endpoint_state_t state) {
	if (state == MOQ_ENDPOINT_ESTABLISHED) {
		printf("[MOQ] Connected %s (draft %u), publishing %s\r\n", mUrl.c_str(),
		       (unsigned)moq_endpoint_negotiated_version(mEndpoint), mNamespace.c_str());
	} else if (state == MOQ_ENDPOINT_DRAINING) {
		printf("[MOQ] Relay sent GOAWAY\r\n");
	} else if (state == MOQ_ENDPOINT_CLOSED) {
		moq_endpoint_terminal_t term;
		memset(&term, 0, sizeof(term));
		moq_endpoint_get_terminal(mEndpoint, &term, sizeof(term));
		unsigned reason = ((unsigned)term.reason < sizeof(CLOSE_REASONS) / sizeof(CLOSE_REASONS[0])) ? term.reason : 0;
		fail(MOQ_ERR_CLOSED, "Session %s (%s, detail=0x%llx)", mUrl.c_str(), CLOSE_REASONS[reason],
		     (unsigned long long)term.detail_code);
	}
}

/* Called with mLock held */
void MoqPublisher::logStats() {
	moq_media_sender_stats_t st;
	memset(&st, 0, sizeof(st));
	moq_result_t rc = moq_media_sender_get_stats(mSender, &st, sizeof(st));
	if (rc != MOQ_OK) {
		fail(rc, "Sender stats");
		return;
	}
	printf("[MOQ] Stats: written=%llu sent=%llu queued=%llu dropped=%llu last_error=%d\r\n",
	       (unsigned long long)st.objects_written, (unsigned long long)st.objects_sent,
	       (unsigned long long)st.objects_queued, (unsigned long long)st.objects_dropped, (int)st.last_error);
}
