/**************************************************************************
 **             __      _________      ______   ____                     **
 **             \ \    / /_   _\ \    / / __ \ / __ \                    **
 **              \ \  / /  | |  \ \  / / |  | | |  | |                   **
 **               \ \/ /   | |   \ \/ /| |  | | |  | |                   **
 **                \  /   _| |_   \  / | |__| | |__| |                   **
 **                 \/   |_____|   \/   \____/ \____/                    **
 ** -------------------------------------------------------------------- **
 **                                                                      **
 ** HungPNQ                                                              **
 **                                                                      **
 ** 12/07/2025                                                           **
 **                                                                      **
 ** SYSTEM V SHARED-MEMORY                                               **
 **                                                                      **
 ** RING BUFFER MEDIA (VIDEO/AUDIO)                                      **
 **                                                                      **
 ** Description: Libraries for mutiples processes access shared media    **
 ** resources                                                            **
 **                                                                      **
 **************************************************************************
 */

#ifndef KIWI_RING_BUFFER_H
#define KIWI_RING_BUFFER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KIWI_RB_MEDIA_PRODUCER = 0x00,
	KIWI_RB_MEDIA_CONSUMER = 0x01,
} KIWI_RB_MEDIA_ROLES;

typedef enum {
    KIWI_RB_MEDIA_ENCODER_FRAME_H264 = 0x00,
    KIWI_RB_MEDIA_ENCODER_FRAME_H265,
    KIWI_RB_MEDIA_ENCODER_FRAME_ALAW,
    KIWI_RB_MEDIA_ENCODER_FRAME_ULAW,
    KIWI_RB_MEDIA_ENCODER_FRAME_AAC,
    KIWI_RB_MEDIA_ENCODER_FRAME_PCM,
} KIWI_RB_MEDIA_ENCODE_TYPE;

typedef enum {
    KIWI_RB_MEDIA_FRAME_HDR_TYPE_I = 0x00,
    KIWI_RB_MEDIA_FRAME_HDR_TYPE_PB,
    KIWI_RB_MEDIA_FRAME_HDR_TYPE_AUDIO,
} KIWI_RB_MEDIA_FRAME_HDR_TYPE;

typedef struct {
    uint32_t id;
    uint8_t *pData;
    uint32_t dataLen;
    uint64_t timestamp;
    uint64_t monotonic;
    uint32_t framePerSeconds;
    KIWI_RB_MEDIA_FRAME_HDR_TYPE type;
    KIWI_RB_MEDIA_ENCODE_TYPE encoder;
} KIWI_RB_MEDIA_FRAMED_S;

typedef void * KIWI_RB_MEDIA_HANDLE_T;

/**
 * @brief Create a shared memory handle, used for communication between processes.
 * @param[in] uri The path to the shared memory.
 * @param[in] role The role of shared memory, either KIWI_RB_MEDIA_PRODUCER or KIWI_RB_MEDIA_CONSUMER.
 * @param[in] wantedSize The size of the shared memory that wanted to be allocated.
 * @return A pointer to shared memory handle, or NULL if failed to allocate.
 */
extern KIWI_RB_MEDIA_HANDLE_T Kiwi_RingBuffer_Create(const char *uri, KIWI_RB_MEDIA_ROLES role, uint32_t wantedSize);

/**
 * @brief Delete a shared memory handle.
 * @param[in] me A pointer to shared memory handle, returned by Kiwi_RingBuffer_Create.
 * @return 0 on success, -1 on failure.
 */
extern int Kiwi_RingBuffer_Delete(KIWI_RB_MEDIA_HANDLE_T me);

/**
 * @brief Send a frame to the ring buffer and store it in shared memory.
 * @param[in] me The ring buffer handle.
 * @param[in] Frame The frame structure contains the data to be written into the ring buffer.
 * @return 0 if success, -1 if error, -2 if data exceed ring buffer size.
 */
extern int Kiwi_RingBuffer_SendTo(KIWI_RB_MEDIA_HANDLE_T me, KIWI_RB_MEDIA_FRAMED_S *Frame);

/**
 * @brief Reads frame from ring buffer
 * @param me The handle of the ring buffer
 * @param Frame The frame to store the read data
 * @return 0 on success, -1 on invalid parameters, -2 on invalid frame magic number, -3 on realloc failure
 */
extern int Kiwi_RingBuffer_ReadFrom(KIWI_RB_MEDIA_HANDLE_T me, KIWI_RB_MEDIA_FRAMED_S *Frame);

/**
 * @brief Wait for data in ring buffer (blocking)
 * @param me Ring buffer handle
 * @return 0 on success, -1 if invalid handle, -2 if not consumer
 * 
 * @note WARNING: 
 * BE CAREFUL USE THIS FUNCTION
 * Process/Thread calls this function must exit gracefully. 
 * If killed while holding the internal mutex, it will trigger 
 * EOWNERDEAD in the PRODUCER. Improper handling by the PRODUCER 
 * may lead to a permanent lock.
 */
extern int Kiwi_RingBuffer_Wait(KIWI_RB_MEDIA_HANDLE_T me);

/**
 * @brief Seeks to the latest frame whose timestamp is less than or equal to provided timestamp.
 * @param[in] me The ring buffer handle.
 * @param[in] timestamp The timestamp to seek to.
 * @return 0 on success, -1 on invalid parameters, -2 on invalid frame magic number, -3 on failure to find frame.
 */
extern int Kiwi_RingBuffer_SeekTo(KIWI_RB_MEDIA_HANDLE_T me, uint64_t wantedTimestamp);

#ifdef __cplusplus
}
#endif

#endif /* KIWI_RING_BUFFER_H */
