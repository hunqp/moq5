#ifndef AAC_ENCODER_H
#define AAC_ENCODER_H

#include <stdint.h>

typedef void (*AacFrameCallback)(void *userData, const uint8_t *frame, uint32_t frameLen);

/**
 * @brief Wraps libfaac to turn 16-bit PCM into ADTS-framed AAC-LC, plus a G.711 A-law
 *        decoder so raw telephony audio (e.g. from the aenc0.shm capture pipeline) can be
 *        transcoded to AAC before being handed to RtmpPublisher::sendAACFrame().
 */
class AacEncoder {
public:
	/**
	 * @param sampleRate PCM sample rate in Hz; must match the A-law source (e.g. 8000).
	 * @param numChannels Number of interleaved channels (1 = mono, matches G.711).
	 * @param bitRate Target total AAC bitrate in bits/sec, split evenly across channels.
	 */
	 AacEncoder(unsigned long sampleRate, unsigned int numChannels, unsigned long bitRate = 32000);
	~AacEncoder();

	AacEncoder(const AacEncoder &) = delete;
	AacEncoder &operator=(const AacEncoder &) = delete;

	/** @brief False if faacEncOpen()/faacEncSetConfiguration() failed; nothing else works then. */
	bool isOpen() const;

	/**
	 * @brief Decode one G.711 A-law/U-law/PCMframe to PCM and feed it into the AAC encoder.
	 * @note Invokes @p onFrame once per complete ADTS AAC access unit produced; a single
	 *       call may yield zero, one, or several frames since A-law frame sizes rarely line
	 *       up with FAAC's fixed input-sample quantum - leftover PCM is queued internally
	 *       and carried over to the next call.
	 */
	void appendPCML(const uint8_t *pcml, uint32_t len, AacFrameCallback onFrame, void *userData);
	void appendULaw(const uint8_t *ulaw, uint32_t len, AacFrameCallback onFrame, void *userData);
	void appendALaw(const uint8_t *alaw, uint32_t len, AacFrameCallback onFrame, void *userData);

private:
	/** @brief Queue one decoded PCM sample; encodes and fires @p onFrame once the queue fills. */
	void lauchFrame(int16_t sample, AacFrameCallback onFrame, void *userData);

	/* faacEncHandle; kept opaque so this header doesn't need <faac.h> */
	void *mHandle;
	int16_t *mPcmQueue;
	uint8_t *mOutFrame;
	unsigned long mPcmQueueLen;
	unsigned long mInputSamples;
	unsigned long mMaxOutputBytes;
};

#endif /* AAC_ENCODER_H */
