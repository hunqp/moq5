#include <stdlib.h>

#include "faac.h"
#include "aac_encoder.h"

/* Classic ITU-T G.711 A-law -> 16-bit linear PCM expansion (the Sun/BSD g711.c algorithm) */
static int16_t aLawDecodeSample(uint8_t aVal) {
	const int SIGN_BIT = 0x80;
	const int QUANT_MASK = 0x0F;
	const int SEG_SHIFT = 4;
	const int SEG_MASK = 0x70;

	aVal ^= 0x55;

	int t = (aVal & QUANT_MASK) << 4;
	int seg = (aVal & SEG_MASK) >> SEG_SHIFT;
	switch (seg) {
	case 0: t += 8;
	break;

	case 1: t += 0x108;
	break;

	default: {
		t += 0x108;
		t <<= (seg - 1);
	}
	break;
	}
	return (int16_t)((aVal & SIGN_BIT) ? t : -t);
}

/* Classic ITU-T G.711 U-law -> 16-bit linear PCM expansion (the Sun/BSD g711.c algorithm) */
static int16_t uLawDecodeSample(uint8_t uVal) {
	const int BIAS = 0x84;

	uVal = (uint8_t)~uVal;

	int sign = uVal & 0x80;
	int exponent = (uVal >> 4) & 0x07;
	int mantissa = uVal & 0x0F;

	int sample = ((mantissa << 3) + BIAS) << exponent;
	sample -= BIAS;

	return (int16_t)(sign ? -sample : sample);
}

AacEncoder::AacEncoder(unsigned long sampleRate, unsigned int numChannels, unsigned long bitRate)
	: mHandle(nullptr), mPcmQueue(nullptr), mOutFrame(nullptr),
	  mPcmQueueLen(0), mInputSamples(0), mMaxOutputBytes(0) {
	faacEncHandle handle = faacEncOpen(sampleRate, numChannels, &mInputSamples, &mMaxOutputBytes);
	if (!handle) {
		return;
	}

	faacEncConfigurationPtr configure = faacEncGetCurrentConfiguration(handle);
	configure->mpegVersion = MPEG4;
	configure->aacObjectType = LOW;
	configure->inputFormat = FAAC_INPUT_16BIT;
	configure->outputFormat = ADTS_STREAM;
	configure->useTns = 0;
	configure->allowMidside = 1;
	configure->bandWidth = 0;
	configure->bitRate = bitRate / (numChannels ? numChannels : 1);
	if (!faacEncSetConfiguration(handle, configure)) {
		faacEncClose(handle);
		return;
	}

	mPcmQueue = (int16_t*)malloc(mInputSamples * sizeof(int16_t));
	mOutFrame = (uint8_t*)malloc(mMaxOutputBytes);
	if (!mPcmQueue || !mOutFrame) {
		free(mPcmQueue);
		free(mOutFrame);
		mPcmQueue = nullptr;
		mOutFrame = nullptr;
		faacEncClose(handle);
		return;
	}

	mHandle = handle;
}

AacEncoder::~AacEncoder() {
	if (mHandle) {
		faacEncClose((faacEncHandle)mHandle);
	}
	free(mPcmQueue);
	free(mOutFrame);
}

bool AacEncoder::isOpen() const { 
	return (mHandle != nullptr) ? true : false; 
}

void AacEncoder::lauchFrame(int16_t sample, AacFrameCallback onFrame, void *userData) {
	mPcmQueue[mPcmQueueLen++] = sample;
	if (mPcmQueueLen < mInputSamples) {
		return;
	}

	int bytesOut = faacEncEncode((faacEncHandle)mHandle, (int32_t*)mPcmQueue, (unsigned int)mInputSamples, mOutFrame, (unsigned int)mMaxOutputBytes);
	mPcmQueueLen = 0;

	if (bytesOut > 0 && onFrame) {
		onFrame(userData, mOutFrame, (uint32_t)bytesOut);
	} else if (bytesOut < 0) {
		;
	}
}

void AacEncoder::appendALaw(const uint8_t *alaw, uint32_t len, AacFrameCallback onFrame, void *userData) {
	if (!mHandle || !alaw) {
		return;
	}

	for (uint32_t i = 0; i < len; ++i) {
		lauchFrame(aLawDecodeSample(alaw[i]), onFrame, userData);
	}
}

void AacEncoder::appendULaw(const uint8_t *ulaw, uint32_t len, AacFrameCallback onFrame, void *userData) {
	if (!mHandle || !ulaw) {
		return;
	}

	for (uint32_t i = 0; i < len; ++i) {
		lauchFrame(uLawDecodeSample(ulaw[i]), onFrame, userData);
	}
}

void AacEncoder::appendPCML(const uint8_t *pcml, uint32_t len, AacFrameCallback onFrame, void *userData) {
	if (!mHandle || !pcml) {
		return;
	}

	/* 16-bit little-endian interleaved PCM, as produced by the capture pipeline */
	uint32_t numSamples = len / 2;
	for (uint32_t i = 0; i < numSamples; ++i) {
		int16_t sample = (int16_t)(pcml[2 * i] | (pcml[2 * i + 1] << 8));
		lauchFrame(sample, onFrame, userData);
	}
}
