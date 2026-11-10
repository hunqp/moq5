#ifndef MOQ_PUBLISHER_H
#define MOQ_PUBLISHER_H

#include <stdint.h>
#include <functional>
#include <string>
#include <mutex>
#include <vector>

#include <moq/cmaf_packager.h>
#include <moq/endpoint.h>
#include <moq/media_sender.h>

#include "codec.h"

/**
 * @brief Publishes one H.264/H.265 video track and one optional AAC-LC audio track (CMAF packaging,
 *        MSF catalog, MoQT draft 22) to a MoQ relay through the libmoq service tier.
 *        Each frame goes out as one CMAF chunk (moof+mdat); the init segments travel
 *        inline in the catalog, which is what the MOQtail player (MSE) consumes.
 * @note setVideoConfig()/setAudioConfig() and the write*() calls are thread-safe, so the
 *       capture threads may call them while the main thread runs start()/stop()/isAlive().
 *       Writes made while no session is up are dropped.
 */
class MoqPublisher {
public:
	/**
	 * @brief Receives every failure inside the publisher.
	 * @param rc libmoq result code (moq_strerror() names it), MOQ_ERR_CLOSED for a lost session.
	 * @param message What failed, with the libmoq error text.
	 * @note Runs on the calling thread (capture or main) with the publisher lock held:
	 *       it must not call back into MoqPublisher.
	 */
	typedef std::function<void(int rc, const std::string &message)> ErrorCallback;

	/**
	 * @param url moqt://host:port[/path] (raw QUIC) or https://host:port/path (WebTransport).
	 * @param ns Slash-separated namespace, e.g. "ipc-media/camera".
	 * @param verifyTls Verify the relay certificate (against caFile when non-empty).
	 */
	MoqPublisher(const std::string &url, const std::string &ns, bool verifyTls, const std::string &caFile);
	~MoqPublisher();

	MoqPublisher(const MoqPublisher &) = delete;
	MoqPublisher &operator=(const MoqPublisher &) = delete;

	/** @brief Replace the default handler (prints to stderr). Call before start(). */
	void setErrorCallback(const ErrorCallback &callback);

	void setVideoConfig(const VideoConfig &config);
	void setAudioConfig(const uint8_t asc[2], uint32_t sampleRate, uint32_t numChannels);
	bool hasVideoConfig();
	bool hasAudioConfig();

	/** @brief Connect, announce the namespace and add the tracks. Requires the video config. */
	bool start();
	/** @brief Flush what is queued (bounded) and tear the session down. */
	void stop();
	/** @brief False once the session is closed or draining; the caller should stop() and start() again.
	 *  @note Logs state changes and, when polled once per second, sender stats every 10 polls. */
	bool isAlive();

	void writeVideo(const uint8_t *data, uint32_t len, bool isKeyFrame, uint64_t ptsUs);
	void writeAudio(const uint8_t *data, uint32_t len, uint64_t ptsUs);

private:
	struct Track {
		const char *name = "";
		moq_media_track_t *track = nullptr;
		moq_cmaf_packager_t *packager = nullptr;
		uint64_t nextDts = 0;  /* CMAF decode timeline in track ticks, never steps backwards */
		uint32_t duration = 0; /* ticks of the next sample */
	};

	bool connect();
	bool openTrack(Track &t, const moq_cmaf_packager_cfg_t &pc, moq_media_track_cfg_t &tc, const char *name);
	void write(Track &t, const uint8_t *data, uint32_t len, bool isSync, uint64_t ptsUs);
	void fail(int rc, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
	void logStateChange(moq_endpoint_state_t state);
	void logStats();

	std::string mUrl;
	std::string mNamespace;
	std::string mCaFile;
	bool mVerifyTls;
	ErrorCallback mOnError;

	VideoConfig mVideoConfig = VideoConfig();
	std::vector<uint8_t> mAsc;
	uint32_t mSampleRate = 0;
	uint32_t mNumChannels = 0;

	std::mutex mLock;
	moq_endpoint_t *mEndpoint = nullptr;
	moq_media_sender_t *mSender = nullptr;
	Track mVideo;
	Track mAudio;
	std::vector<uint8_t> mVideoSample;
	/* Timestamps are taken relative to mOriginUs, set per session and shared by both tracks */
	uint64_t mOriginUs = 0;
	uint64_t mLastVideoPtsUs = 0;
	moq_endpoint_state_t mLastState = MOQ_ENDPOINT_CONNECTING;
	unsigned int mAlivePolls = 0;
};

#endif /* MOQ_PUBLISHER_H */
