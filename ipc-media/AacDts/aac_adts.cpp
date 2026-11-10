#include "aac_adts.h"

/* MPEG-4 sampling_frequency_index table (ISO/IEC 13818-7 / 14496-3), indices 13-15 reserved */
static const uint32_t kAacSampleRateTable[13] = {
	96000, 88200, 64000, 48000, 44100, 32000, 24000,
	22050, 16000, 12000, 11025, 8000,  7350,
};

bool AacAdts::isAdtsFrame(const uint8_t *data, uint32_t len) {
	if (!data || len < 7) {
		return false;
	}
	/* 12-bit ADTS sync word: 0xFFF, with the low bit of byte[1] reserved for MPEG version */
	return data[0] == 0xFF && (data[1] & 0xF0) == 0xF0;
}

void AacAdts::stripAdtsHeader(const uint8_t *data, uint32_t len, const uint8_t **payload, uint32_t *payloadLen) {
	if (!isAdtsFrame(data, len)) {
		*payload = data;
		*payloadLen = len;
		return;
	}

	bool protectionAbsent = (data[1] & 0x01) != 0;
	uint32_t headerLen = protectionAbsent ? 7 : 9;
	if (headerLen >= len) {
		*payload = data;
		*payloadLen = 0;
		return;
	}
	*payload = data + headerLen;
	*payloadLen = len - headerLen;
}

/* Packs (AAC-LC) audioObjectType/samplingFreqIndex/channelConfig into the 2-byte ASC form */
static void packAudioSpecificConfig(uint8_t audioObjectType, uint8_t samplingFreqIndex,
									 uint8_t channelConfig, uint8_t asc[2]) {
	asc[0] = (uint8_t)((audioObjectType << 3) | (samplingFreqIndex >> 1));
	asc[1] = (uint8_t)(((samplingFreqIndex & 0x01) << 7) | (channelConfig << 3));
}

bool AacAdts::buildAudioSpecificConfigFromAdts(const uint8_t *data, uint32_t len, uint8_t asc[2]) {
	if (!isAdtsFrame(data, len)) {
		return false;
	}

	uint8_t adtsProfile = (data[2] >> 6) & 0x03;      /* audioObjectType - 1 */
	uint8_t samplingFreqIndex = (data[2] >> 2) & 0x0F;
	uint8_t channelConfig = (uint8_t)(((data[2] & 0x01) << 2) | ((data[3] >> 6) & 0x03));

	if (samplingFreqIndex >= sizeof(kAacSampleRateTable) / sizeof(kAacSampleRateTable[0])) {
		return false;
	}

	packAudioSpecificConfig((uint8_t)(adtsProfile + 1), samplingFreqIndex, channelConfig, asc);
	return true;
}
