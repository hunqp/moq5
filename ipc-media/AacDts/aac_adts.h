#ifndef AAC_ADTS_H
#define AAC_ADTS_H

#include <stdint.h>

/** @brief Stateless ADTS helpers; every method operates only on its arguments. */
class AacAdts {
public:
	/** @brief Returns true if @p data starts with a 12-bit ADTS sync word (0xFFF). */
	static bool isAdtsFrame(const uint8_t *data, uint32_t len);

	/**
	 * @brief Strip the ADTS header (if present) from one AAC frame.
	 * @param[in] data Frame as read from the encoder; may or may not carry an ADTS header.
	 * @param[in] len Length of data.
	 * @param[out] payload Receives a pointer into @p data at the start of the raw AAC payload.
	 * @param[out] payloadLen Receives the length of the raw AAC payload.
	 * @note If no ADTS header is present, payload/payloadLen simply echo data/len unchanged.
	 */
	static void stripAdtsHeader(const uint8_t *data, uint32_t len, const uint8_t **payload, uint32_t *payloadLen);

	/**
	 * @brief Build the 2-byte MPEG-4 AudioSpecificConfig FLV expects in the AAC sequence header,
	 *        reading the object type / sample rate / channel count out of an ADTS header.
	 * @param[out] asc Destination for the 2 ASC bytes.
	 * @return true on success, false if @p data is not a valid ADTS header.
	 */
	static bool buildAudioSpecificConfigFromAdts(const uint8_t *data, uint32_t len, uint8_t asc[2]);
};

#endif /* AAC_ADTS_H */
