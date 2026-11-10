#ifndef CODEC_H
#define CODEC_H

#include <stdint.h>
#include <string>
#include <vector>

enum VideoCodec {
	VIDEO_CODEC_H264, /* record is an avcC, CMAF sample entry 'avc1' */
	VIDEO_CODEC_H265  /* record is an hvcC, CMAF sample entry 'hvc1' */
};

/** @brief Everything a CMAF video track needs from the in-band parameter sets. */
struct VideoConfig {
	VideoCodec codec;
	std::string codecString;     /* RFC 6381, e.g. "avc1.640028" or "hvc1.1.6.L93.B0" */
	std::vector<uint8_t> record; /* avcC/hvcC with 4-byte NAL lengths */
	uint32_t width;              /* cropped picture size from the SPS */
	uint32_t height;
};

/** @brief Stateless H.264/H.265 helpers. Access units may be 4-byte length prefixed or Annex-B. */
namespace Codec {

/**
 * @brief Build the decoder configuration from the parameter sets carried in one access unit
 *        (H.264: SPS+PPS, H.265: VPS+SPS+PPS).
 * @return false if a parameter set is missing or malformed.
 */
bool buildVideoConfig(VideoCodec codec, const uint8_t *au, uint32_t len, VideoConfig &config);

/**
 * @brief Convert one access unit into a CMAF sample: 4-byte length prefixed NAL units without
 *        parameter sets and AUD, which 'avc1'/'hvc1' carry only in the init segment.
 */
void toCmafSample(VideoCodec codec, const uint8_t *au, uint32_t len, std::vector<uint8_t> &sample);

} /* namespace Codec */

#endif /* CODEC_H */
