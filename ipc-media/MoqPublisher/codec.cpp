#include <assert.h>
#include <algorithm>
#include <iterator>

#include <moq/codec_signaling.h>

#include "codec.h"

namespace {

const uint32_t LENGTH_SIZE = 4;
const uint8_t START_CODE[] = {0, 0, 0, 1};

struct NalUnit {
	const uint8_t *data;
	uint32_t len;
};

/* Exp-Golomb reader over an RBSP (emulation prevention bytes already removed) */
class BitReader {
public:
	explicit BitReader(const std::vector<uint8_t> &rbsp) : mData(rbsp), mPos(0) {}

	bool overrun() const { return mPos > mData.size() * 8; }

	void skip(uint32_t n) { mPos += n; }

	uint32_t bits(uint32_t n) {
		uint32_t v = 0;
		while (n--) {
			uint32_t bit = (mPos < mData.size() * 8) ? (mData[mPos / 8] >> (7 - mPos % 8)) & 1 : 0;
			v = (v << 1) | bit;
			++mPos;
		}
		return v;
	}

	uint32_t ue() {
		uint32_t zeros = 0;
		while (bits(1) == 0 && zeros < 32 && !overrun()) {
			++zeros;
		}
		return ((1u << zeros) - 1) + bits(zeros);
	}

	int32_t se() {
		uint32_t k = ue();
		return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
	}

private:
	const std::vector<uint8_t> &mData;
	size_t mPos;
};

/* ---- SPS parsers: the reader starts right after the NAL header ---- */

void skipScalingList(BitReader &br, int size) {
	int last = 8, next = 8;
	for (int i = 0; i < size; ++i) {
		if (next != 0) {
			next = (last + br.se() + 256) % 256;
		}
		last = (next == 0) ? last : next;
	}
}

bool parseH264Sps(BitReader &br, uint32_t &width, uint32_t &height) {
	static const uint32_t CHROMA_PROFILES[] = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};
	uint32_t profile = br.bits(8);
	br.skip(16); /* constraint flags, level */
	br.ue();     /* seq_parameter_set_id */
	uint32_t chromaFormat = 1;
	if (std::find(std::begin(CHROMA_PROFILES), std::end(CHROMA_PROFILES), profile) != std::end(CHROMA_PROFILES)) {
		chromaFormat = br.ue();
		br.skip(chromaFormat == 3); /* separate_colour_plane_flag */
		br.ue();                    /* bit_depth_luma_minus8 */
		br.ue();                    /* bit_depth_chroma_minus8 */
		br.skip(1);                 /* qpprime_y_zero_transform_bypass_flag */
		if (br.bits(1)) {
			int lists = (chromaFormat == 3) ? 12 : 8;
			for (int i = 0; i < lists; ++i) {
				if (br.bits(1)) {
					skipScalingList(br, i < 6 ? 16 : 64);
				}
			}
		}
	}
	br.ue(); /* log2_max_frame_num_minus4 */
	uint32_t pocType = br.ue();
	if (pocType == 0) {
		br.ue();
	} else if (pocType == 1) {
		br.skip(1);
		br.se();
		br.se();
		uint32_t cycle = br.ue();
		for (uint32_t i = 0; i < cycle && !br.overrun(); ++i) {
			br.se();
		}
	}
	br.ue();    /* max_num_ref_frames */
	br.skip(1); /* gaps_in_frame_num_value_allowed_flag */
	uint32_t widthMbs = br.ue() + 1;
	uint32_t heightMapUnits = br.ue() + 1;
	uint32_t frameMbsOnly = br.bits(1);
	br.skip(!frameMbsOnly); /* mb_adaptive_frame_field_flag */
	br.skip(1);             /* direct_8x8_inference_flag */
	uint32_t crop[4] = {0, 0, 0, 0}; /* left, right, top, bottom */
	if (br.bits(1)) {
		for (uint32_t &c : crop) {
			c = br.ue();
		}
	}
	uint32_t cropUnitX = (chromaFormat == 1 || chromaFormat == 2) ? 2 : 1;
	uint32_t cropUnitY = ((chromaFormat == 1) ? 2 : 1) * (2 - frameMbsOnly);
	width = widthMbs * 16 - (crop[0] + crop[1]) * cropUnitX;
	height = heightMapUnits * 16 * (2 - frameMbsOnly) - (crop[2] + crop[3]) * cropUnitY;
	return !br.overrun();
}

bool parseH265Sps(BitReader &br, uint32_t &width, uint32_t &height) {
	br.skip(4); /* sps_video_parameter_set_id */
	uint32_t maxSubLayersMinus1 = br.bits(3);
	br.skip(1);  /* sps_temporal_id_nesting_flag */
	br.skip(96); /* general profile_tier_level */
	bool subProfile[8] = {false}, subLevel[8] = {false};
	for (uint32_t i = 0; i < maxSubLayersMinus1; ++i) {
		subProfile[i] = br.bits(1);
		subLevel[i] = br.bits(1);
	}
	br.skip(maxSubLayersMinus1 ? 2 * (8 - maxSubLayersMinus1) : 0); /* reserved_zero_2bits */
	for (uint32_t i = 0; i < maxSubLayersMinus1; ++i) {
		br.skip(subProfile[i] ? 88 : 0);
		br.skip(subLevel[i] ? 8 : 0);
	}
	br.ue(); /* sps_seq_parameter_set_id */
	uint32_t chromaFormat = br.ue();
	br.skip(chromaFormat == 3); /* separate_colour_plane_flag */
	uint32_t lumaWidth = br.ue();
	uint32_t lumaHeight = br.ue();
	uint32_t conf[4] = {0, 0, 0, 0}; /* left, right, top, bottom */
	if (br.bits(1)) {
		for (uint32_t &c : conf) {
			c = br.ue();
		}
	}
	uint32_t subWidth = (chromaFormat == 1 || chromaFormat == 2) ? 2 : 1;
	uint32_t subHeight = (chromaFormat == 1) ? 2 : 1;
	width = lumaWidth - subWidth * (conf[0] + conf[1]);
	height = lumaHeight - subHeight * (conf[2] + conf[3]);
	return !br.overrun();
}

/* ---- Per-codec NAL layout and libmoq signaling ---- */

struct Traits {
	uint32_t headerLen; /* NAL header bytes */
	uint8_t typeShift;
	uint8_t typeMask;
	uint8_t paramSetFirst; /* parameter set types are contiguous: H.264 SPS..PPS, H.265 VPS..PPS */
	uint8_t paramSetLast;
	uint8_t sps;
	uint8_t aud;
	moq_codec_source_format_t source;
	moq_codec_config_format_t format;
	const char *entry;
	bool (*parseSps)(BitReader &, uint32_t &, uint32_t &);
};

const Traits TRAITS[] = {
	/* VIDEO_CODEC_H264 */ {1, 0, 0x1F, 7, 8, 7, 9, MOQ_CODEC_SOURCE_AVC_ANNEXB, MOQ_CODEC_CONFIG_AVCC, "avc1", parseH264Sps},
	/* VIDEO_CODEC_H265 */ {2, 1, 0x3F, 32, 34, 33, 35, MOQ_CODEC_SOURCE_HEVC_ANNEXB, MOQ_CODEC_CONFIG_HVCC, "hvc1", parseH265Sps},
};

const Traits &traits(VideoCodec codec) {
	assert(codec == VIDEO_CODEC_H264 || codec == VIDEO_CODEC_H265);
	return TRAITS[codec];
}

uint8_t nalType(const Traits &t, const NalUnit &nal) {
	return (nal.data[0] >> t.typeShift) & t.typeMask;
}

uint32_t startCodeLength(const uint8_t *p, uint32_t remain) {
	if (remain >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
		return 4;
	}
	return (remain >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) ? 3 : 0;
}

void splitAnnexB(const uint8_t *au, uint32_t len, std::vector<NalUnit> &nals) {
	const uint8_t *nalStart = nullptr;
	for (uint32_t i = 0; i < len;) {
		uint32_t scLen = startCodeLength(au + i, len - i);
		if (scLen == 0) {
			++i;
			continue;
		}
		if (nalStart) {
			nals.push_back({nalStart, (uint32_t)(au + i - nalStart)});
		}
		i += scLen;
		nalStart = au + i;
	}
	if (nalStart) {
		nals.push_back({nalStart, (uint32_t)(au + len - nalStart)});
	}
}

/* @return true if the length prefixes describe the whole access unit exactly */
bool splitLengthPrefixed(const uint8_t *au, uint32_t len, std::vector<NalUnit> &nals) {
	uint32_t i = 0;
	while (i + LENGTH_SIZE <= len) {
		uint32_t nalLen = ((uint32_t)au[i] << 24) | ((uint32_t)au[i + 1] << 16) | ((uint32_t)au[i + 2] << 8) | au[i + 3];
		i += LENGTH_SIZE;
		if (nalLen == 0 || nalLen > len - i) {
			return false;
		}
		nals.push_back({au + i, nalLen});
		i += nalLen;
	}
	return i == len;
}

/* A 4-byte length of 256..511 reads 00 00 01 xx, like a start code, so length prefixes are tried
 * first. NAL units shorter than their header are dropped. */
std::vector<NalUnit> splitAccessUnit(const Traits &t, const uint8_t *au, uint32_t len) {
	std::vector<NalUnit> nals;
	if (!splitLengthPrefixed(au, len, nals)) {
		nals.clear();
		splitAnnexB(au, len, nals);
	}
	nals.erase(std::remove_if(nals.begin(), nals.end(), [&t](const NalUnit &n) { return n.len < t.headerLen; }), nals.end());
	return nals;
}

const NalUnit *findNal(const Traits &t, const std::vector<NalUnit> &nals, uint8_t type) {
	auto it = std::find_if(nals.begin(), nals.end(), [&](const NalUnit &n) { return nalType(t, n) == type; });
	return (it == nals.end()) ? nullptr : &*it;
}

/* Strip emulation prevention bytes (00 00 03) */
std::vector<uint8_t> toRbsp(const uint8_t *begin, const uint8_t *end) {
	std::vector<uint8_t> rbsp;
	uint32_t zeros = 0;
	for (const uint8_t *p = begin; p < end; ++p) {
		if (zeros >= 2 && *p == 0x03) {
			zeros = 0;
			continue;
		}
		zeros = (*p == 0) ? zeros + 1 : 0;
		rbsp.push_back(*p);
	}
	return rbsp;
}

bool buildRecord(const Traits &t, const std::vector<uint8_t> &annexB, std::vector<uint8_t> &record) {
	moq_codec_init_data_cfg_t cfg;
	moq_codec_init_data_cfg_init(&cfg);
	cfg.source_format = t.source;
	cfg.source.data = annexB.data();
	cfg.source.len = annexB.size();
	cfg.nal.length_size = LENGTH_SIZE;
	size_t len = 0;
	if (moq_codec_init_data_build(&cfg, NULL, 0, &len) != MOQ_ERR_BUFFER) {
		return false;
	}
	record.resize(len);
	return moq_codec_init_data_build(&cfg, record.data(), record.size(), &len) == MOQ_OK;
}

bool formatCodecString(const Traits &t, const std::vector<uint8_t> &record, std::string &codecString) {
	moq_codec_string_cfg_t cfg;
	moq_codec_string_cfg_init(&cfg);
	cfg.config_format = t.format;
	cfg.sample_entry.data = (const uint8_t*)t.entry;
	cfg.sample_entry.len = 4;
	cfg.decoder_config.data = record.data();
	cfg.decoder_config.len = record.size();
	char buf[64];
	size_t len = 0;
	if (moq_codec_string_format(&cfg, (uint8_t*)buf, sizeof(buf), &len) != MOQ_OK) {
		return false;
	}
	codecString.assign(buf, len);
	return true;
}

} /* namespace */

bool Codec::buildVideoConfig(VideoCodec codec, const uint8_t *au, uint32_t len, VideoConfig &config) {
	const Traits &t = traits(codec);
	std::vector<NalUnit> nals = splitAccessUnit(t, au, len);

	/* libmoq assembles the record from an Annex-B stream of the parameter sets */
	std::vector<uint8_t> annexB;
	for (uint8_t type = t.paramSetFirst; type <= t.paramSetLast; ++type) {
		const NalUnit *nal = findNal(t, nals, type);
		if (!nal) {
			return false;
		}
		annexB.insert(annexB.end(), std::begin(START_CODE), std::end(START_CODE));
		annexB.insert(annexB.end(), nal->data, nal->data + nal->len);
	}
	config.codec = codec;
	if (!buildRecord(t, annexB, config.record) || !formatCodecString(t, config.record, config.codecString)) {
		return false;
	}

	const NalUnit *sps = findNal(t, nals, t.sps);
	std::vector<uint8_t> rbsp = toRbsp(sps->data + t.headerLen, sps->data + sps->len);
	BitReader br(rbsp);
	return t.parseSps(br, config.width, config.height) && config.width > 0 && config.height > 0;
}

void Codec::toCmafSample(VideoCodec codec, const uint8_t *au, uint32_t len, std::vector<uint8_t> &sample) {
	const Traits &t = traits(codec);
	sample.clear();
	for (const NalUnit &nal : splitAccessUnit(t, au, len)) {
		uint8_t type = nalType(t, nal);
		if ((type >= t.paramSetFirst && type <= t.paramSetLast) || type == t.aud) {
			continue;
		}
		const uint8_t prefix[] = {(uint8_t)(nal.len >> 24), (uint8_t)(nal.len >> 16), (uint8_t)(nal.len >> 8), (uint8_t)nal.len};
		sample.insert(sample.end(), std::begin(prefix), std::end(prefix));
		sample.insert(sample.end(), nal.data, nal.data + nal.len);
	}
}
