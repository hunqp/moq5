/* C/C++ include headers */
#include <string>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

/* User include headers */
#include "aac_adts.h"
#include "aac_encoder.h"
#include "codec.h"
#include "moq_publisher.h"
#include "kiwi_ringbuffer.h"

static const struct {
    /* Share-memory to get media samples */
    char *stVideoPath = (char*)"/tmp/venc1.shm";
    char *stAudioPath = (char*)"/tmp/aenc0.shm";

    /* Attributes */
    bool mute = false;
    bool tlsSupport = false; /* This will be enable/disable SSL_VERIFY_HOST & SSL_VERIFY_PEER */
    std::string certificate = ""; /* If tlsSupport TRUE, this MUST NOT empty */
    int reconnectAfterSecs = 3;

    /* Media configurations */
    struct {
        char *vCodec = (char*)"H264";
        char *aCodec = (char*)"AAC";
        int aSamplesrate = 8000UL;
        int aNumChannels = 1UL;
        int aSamplesPerFrame = 1024;
        int aAgeLogEveryFrames = 40; /* ~5 s at 8 kHz */
    } sMediaConfigs;

    /* Publish domain */
    /* MOQtail relay (draft 22); view on https://player.moqtail.dev */
    std::string stEndpoint  = "moqt://relay.moqtail.dev:443";
    std::string stNamespace = "livestream";
} sDefautlConfiguration;

static volatile sig_atomic_t sEscape = false;

static MoqPublisher sPublisher(
    sDefautlConfiguration.stEndpoint, 
    sDefautlConfiguration.stNamespace, 
    sDefautlConfiguration.tlsSupport, 
    sDefautlConfiguration.certificate
);

static const auto &sMediaConfigs = sDefautlConfiguration.sMediaConfigs;
/* vCodec picks which ring buffer encoding is published: "H264" or "H265" */
static const bool sHevc = (strcmp(sMediaConfigs.vCodec, "H265") == 0);

struct AudioTimeline {
	uint64_t anchorUs;
	uint64_t frameCount;
};

static void sigProc(int signal) {
	(void)signal;
	sEscape = true;
}

/* Wall-clock microseconds, the clock of Frame.timestamp; used to log how old media is when it is sent */
static uint64_t nowUs() {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void publishVideoFrame(const KIWI_RB_MEDIA_FRAMED_S &Frame, bool idr) {
	if (Frame.encoder != (sHevc ? KIWI_RB_MEDIA_ENCODER_FRAME_H265 : KIWI_RB_MEDIA_ENCODER_FRAME_H264)) {
		static bool warned = false;
		if (!warned) {
			printf("Video encoder %d does not match vCodec %s, frames dropped\r\n", Frame.encoder, sMediaConfigs.vCodec);
			warned = true;
		}
		return;
	}
	if (idr && !sPublisher.hasVideoConfig()) {
		VideoConfig config;
		if (Codec::buildVideoConfig(sHevc ? VIDEO_CODEC_H265 : VIDEO_CODEC_H264, Frame.pData, Frame.dataLen, config)) {
			printf("[MOQ] Video %s %ux%u\r\n", config.codecString.c_str(), config.width, config.height);
			sPublisher.setVideoConfig(config);
		} else {
			printf("[MOQ] IDR without valid parameter sets, waiting for the next one\r\n");
		}
	}
	/* Frame.timestamp is the capture time (wall-clock us), which LOC carries as the capture timestamp */
	sPublisher.writeVideo(Frame.pData, Frame.dataLen, idr, Frame.timestamp);
}

static void *getVideoSamples(void *args) {
    KIWI_RB_MEDIA_HANDLE_T consumer = Kiwi_RingBuffer_Create(sDefautlConfiguration.stVideoPath, KIWI_RB_MEDIA_CONSUMER, (250 * 1025));
    assert(consumer);

    while (!sEscape) {
        KIWI_RB_MEDIA_FRAMED_S Frame = {0};
		int rc = Kiwi_RingBuffer_ReadFrom(consumer, &Frame);
		if (rc != 0) {
			usleep(5000);
			continue;
		}
		bool idr = (Frame.type == KIWI_RB_MEDIA_FRAME_HDR_TYPE_I) ? true : false;
		if (idr) {
			printf("IDR Frame size = %d | codec = %d | timestamp = %llu | age = %lld ms\r\n", Frame.dataLen, Frame.encoder, Frame.timestamp,
			       ((long long)nowUs() - (long long)Frame.timestamp) / 1000);
		}
        /* This Frame output as AVCC Style [Prefix length][Payload data] */
        publishVideoFrame(Frame, idr);
    }
    Kiwi_RingBuffer_Delete(consumer);

    return NULL;
}

static void aacAdtsFrameCompleted(void *userData, const uint8_t *pFrame, uint32_t FrameLen) {
	if (sDefautlConfiguration.mute) {
		return;
	}
	AudioTimeline *timeline = (AudioTimeline*)userData;
	if (!sPublisher.hasAudioConfig()) {
		uint8_t asc[2];
		if (!AacAdts::buildAudioSpecificConfigFromAdts(pFrame, FrameLen, asc)) {
			return;
		}
		sPublisher.setAudioConfig(asc, sMediaConfigs.aSamplesrate, sMediaConfigs.aNumChannels);
	}
	uint64_t ptsUs = timeline->anchorUs + timeline->frameCount * sMediaConfigs.aSamplesPerFrame * 1000000ULL / sMediaConfigs.aSamplesrate;
	timeline->frameCount++;
	if (timeline->frameCount % sMediaConfigs.aAgeLogEveryFrames == 0) {
		printf("AAC frame age = %lld ms\r\n", ((long long)nowUs() - (long long)ptsUs) / 1000);
	}

	/* MoQ/LOC carries raw AAC access units; the decoder config (ASC) travels in the catalog */
	const uint8_t *payload = NULL;
	uint32_t payloadLen = 0;
	AacAdts::stripAdtsHeader(pFrame, FrameLen, &payload, &payloadLen);
	if (payloadLen > 0) {
		sPublisher.writeAudio(payload, payloadLen, ptsUs);
	}
}

static void *getAudioSamples(void *args) {
    KIWI_RB_MEDIA_HANDLE_T consumer = Kiwi_RingBuffer_Create(sDefautlConfiguration.stAudioPath, KIWI_RB_MEDIA_CONSUMER, (250 * 1025));
    assert(consumer);

    /* Transcode ALAW/ULAW/PCML to AAC before streaming */
	AacEncoder aacEncoder(sMediaConfigs.aSamplesrate, sMediaConfigs.aNumChannels);
	assert(aacEncoder.isOpen());
	AudioTimeline timeline = {0, 0};

    while (!sEscape) {
        KIWI_RB_MEDIA_FRAMED_S Frame = {0};
		int rc = Kiwi_RingBuffer_ReadFrom(consumer, &Frame);
		if (rc != 0) {
			usleep(5000);
			continue;
		}
        /* AAC frames are timed from the capture time of the first PCM sample */
        if (timeline.anchorUs == 0) {
            timeline.anchorUs = Frame.timestamp;
        }
        /* Change into AAC */
        if (Frame.encoder == KIWI_RB_MEDIA_ENCODER_FRAME_ALAW) {
			aacEncoder.appendALaw(Frame.pData, Frame.dataLen, aacAdtsFrameCompleted, &timeline);
		} else if (Frame.encoder == KIWI_RB_MEDIA_ENCODER_FRAME_ULAW) {
			aacEncoder.appendULaw(Frame.pData, Frame.dataLen, aacAdtsFrameCompleted, &timeline);
		} else if (Frame.encoder == KIWI_RB_MEDIA_ENCODER_FRAME_PCM) {
			aacEncoder.appendPCML(Frame.pData, Frame.dataLen, aacAdtsFrameCompleted, &timeline);
		}
    }
    Kiwi_RingBuffer_Delete(consumer);

    return NULL;
}

/* Tracks are announced once their decoder configs are known (first IDR / first AAC frame) */
static bool isMediaConfigured() {
    return sPublisher.hasVideoConfig() && (sDefautlConfiguration.mute || sPublisher.hasAudioConfig());
}

static void runLoop() {
    while (!sEscape && !isMediaConfigured()) {
        usleep(100000);
    }
    while (!sEscape) {
        if (sPublisher.start()) {
            while (!sEscape && sPublisher.isAlive()) {
                sleep(1);
            }
            sPublisher.stop();
        }
        if (!sEscape) {
            printf("[MOQ] Session ended, reconnecting in %d seconds\r\n", sDefautlConfiguration.reconnectAfterSecs);
            sleep(sDefautlConfiguration.reconnectAfterSecs);
        }
    }
}

/* Every publisher failure ends up here; rc is the libmoq result code (moq_strerror() names it) */
static void onPublisherError(int rc, const std::string &message) {
	printf("[MOQ][ERROR] %s\r\n", message.c_str());
	(void)rc;
}

int main() {
    assert(sHevc || strcmp(sMediaConfigs.vCodec, "H264") == 0);
    assert(strcmp(sMediaConfigs.aCodec, "AAC") == 0); /* audio is always published as AAC-LC */
    assert(!sDefautlConfiguration.tlsSupport || !sDefautlConfiguration.certificate.empty());

    signal(SIGINT , sigProc);
    signal(SIGTERM, sigProc);
    sPublisher.setErrorCallback(onPublisherError);

    pthread_t video = 0, audio = 0;
    pthread_create(&video, NULL, getVideoSamples, NULL);
    pthread_create(&audio, NULL, getAudioSamples, NULL);
    assert(video && audio);

    runLoop();

    pthread_join(video, NULL);
    pthread_join(audio, NULL);

    return 0;
}
