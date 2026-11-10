#include <stdio.h>
#include <errno.h>
#include <stddef.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <pthread.h>

#include "kiwi_ringbuffer.h"

#define KIWI_SYNCHRONOUS_ENABLE	 (0)
#define KIWI_NUMBYTES_ALIGNMENT	 (4)	
#define KIWI_FRAMER_HDR_MAGICNUM (0x01234567)

typedef struct {
	/* Those variables are used for shared memory attributes */
	int sId;
	void *addrParentPtr;
	uint32_t addrParentSize;
	KIWI_RB_MEDIA_ROLES role;

	/* Only use for CONSUMER, preallocate memory, purpose avoid 
	 * calls malloc()/free() many times 
	 */
	uint8_t *bufferPreallocatePtr;
	uint32_t bufferPreallocateSize;

	/* Those variables are used for managing usabled memory */
	uint32_t sequence;
	uint32_t memSizeUsable;
	uint8_t *addrOfHeadPtr;
	uint8_t *addrOfTailPtr;
	uint8_t *addrOfRuntimePtr;
} RB_SHM_CONTEXT_S;

typedef struct {
	uint32_t addrParentSize;
	uint32_t sequence;
	/* Track to the current index in array has written data recently */
	uint32_t commitId;
	/* Using to track index has been written recently */
	uint32_t runtimId;
	/* Synchronization mechanism variables */
	pthread_cond_t signaling;
	pthread_mutex_t semaphor;
} RB_SHM_HDR_S;

typedef struct {
	uint32_t id;
	uint32_t sequence;
	uint32_t nextIndexStep;
	uint32_t previousIndexStep;
	uint32_t length;
	uint32_t magicNum;
	uint32_t uniqeNum;
	uint64_t timestamp;
	uint64_t monotonic;
	uint32_t framePerSeconds;
	KIWI_RB_MEDIA_FRAME_HDR_TYPE type;
    KIWI_RB_MEDIA_ENCODE_TYPE encoder;
} RB_FRAMER_HDR_S;

/*-----------------------------------------------------------------------------------*/

static uint32_t Kiwi_ATOM_READ_U32(uint32_t *u32ValuePtr);
static void Kiwi_ATOM_WRITE_U32(uint32_t *u32ValuePtr, uint32_t u32Value);

/*-----------------------------------------------------------------------------------*/

static void* Kiwi_SHM_Open(int *sId, const char *uri, uint32_t wantedSize, bool readOnly);
static void  Kiwi_SHM_Close(int sId, void *pSHM, bool erase);
static inline uint32_t Kiwi_HDR_GenerateUniqueCode(RB_FRAMER_HDR_S *fHdr);
static inline void Kiwi_HDR_SetCommit(RB_SHM_HDR_S *pSHM_HDR, uint32_t  u32Index, uint32_t  u32Sequence);
static inline void Kiwi_HDR_GetCommit(RB_SHM_HDR_S *pSHM_HDR, uint32_t *u32Index, uint32_t *u32Sequence);
static inline bool Kiwi_HDR_IsValidFrame(RB_SHM_CONTEXT_S *pSHM, RB_FRAMER_HDR_S *fHdr);

/*-----------------------------------------------------------------------------------*/

/**
 * @brief Create a Kiwi Ring Buffer shared memory context.
 * @param [in] uri The URI of the shared memory.
 * @param [in] role The role of the process accessing the shared memory.
 * @param [in] wantedSize The size of preallocated memory for the consumer.
 * @return A pointer to the RB_SHM_CONTEXT_S structure.
 * @note The created shared memory context is valid until Kiwi_RingBuffer_Delete is called.
 * @note The created shared memory context is not valid until Kiwi_RingBuffer_Delete is called.
 */
void * Kiwi_RingBuffer_Create(const char *uri, KIWI_RB_MEDIA_ROLES role, uint32_t wantedSize) {
	RB_SHM_CONTEXT_S *pSHM = (RB_SHM_CONTEXT_S*)malloc(sizeof(RB_SHM_CONTEXT_S));
	if (!pSHM) {
		return NULL;
	}

	/* -------------------------------- KIWI_RB_MEDIA_PRODUCER -------------------------------- */
	if (role == KIWI_RB_MEDIA_PRODUCER) {
		pSHM->addrParentPtr = Kiwi_SHM_Open(&pSHM->sId, uri, wantedSize, false);
		if (!pSHM->addrParentPtr) {
			free(pSHM);
			return NULL;
		}
		pSHM->addrParentSize = wantedSize;
		pSHM->role = KIWI_RB_MEDIA_PRODUCER;
		pSHM->bufferPreallocatePtr = NULL;
		pSHM->bufferPreallocateSize = 0;
		pSHM->addrOfHeadPtr = (uint8_t*)pSHM->addrParentPtr + sizeof(RB_SHM_HDR_S);
		pSHM->addrOfTailPtr = (uint8_t*)pSHM->addrParentPtr + pSHM->addrParentSize;
		pSHM->addrOfRuntimePtr = pSHM->addrOfHeadPtr;
		pSHM->memSizeUsable = pSHM->addrOfTailPtr - pSHM->addrOfHeadPtr;
		Kiwi_ATOM_WRITE_U32(&pSHM->sequence, 0);

		/**
		 * Initialise SHM Header 
		 * It will contain all information about new data,
		 * mechanism to track commit and runtime index and
		 * share waiting/singaling for all consumers
		 */
		RB_SHM_HDR_S *pSHM_HDR = (RB_SHM_HDR_S*)pSHM->addrParentPtr;
		Kiwi_ATOM_WRITE_U32(&pSHM_HDR->commitId, 0);
		Kiwi_ATOM_WRITE_U32(&pSHM_HDR->runtimId, 0);
		pSHM_HDR->addrParentSize = pSHM->addrParentSize;
		/* Initialise semaphore and condition variable */
		#if KIWI_SYNCHRONOUS_ENABLE
		pthread_mutexattr_t attrSemaphor = {0};
		pthread_condattr_t attrSignaling = {0};
		pthread_mutexattr_init(&attrSemaphor);
		pthread_mutexattr_setrobust(&attrSemaphor, PTHREAD_MUTEX_ROBUST);
		pthread_mutexattr_setpshared(&attrSemaphor, PTHREAD_PROCESS_SHARED);
		pthread_condattr_init(&attrSignaling);
		pthread_condattr_setpshared(&attrSignaling, PTHREAD_PROCESS_SHARED);
		pthread_cond_init(&pSHM_HDR->signaling, &attrSignaling);
		pthread_mutex_init(&pSHM_HDR->semaphor, &attrSemaphor);
		#endif
	}
	/* -------------------------------- KIWI_RB_MEDIA_CONSUMER -------------------------------- */
	else { 
		pSHM->addrParentPtr = Kiwi_SHM_Open(&pSHM->sId, uri, 0, true);
		if (!pSHM->addrParentPtr) {
			free(pSHM);
			return NULL;
		}
		RB_SHM_HDR_S *pSHM_HDR = (RB_SHM_HDR_S*)pSHM->addrParentPtr;
		pSHM->addrParentSize = pSHM_HDR->addrParentSize;
		pSHM->role = KIWI_RB_MEDIA_CONSUMER;
		pSHM->addrOfHeadPtr = (uint8_t*)pSHM->addrParentPtr + sizeof(RB_SHM_HDR_S);
		pSHM->addrOfTailPtr = (uint8_t*)pSHM->addrParentPtr + pSHM->addrParentSize;
		pSHM->memSizeUsable = pSHM->addrOfTailPtr - pSHM->addrOfHeadPtr;
		pSHM->addrOfRuntimePtr = pSHM->addrOfHeadPtr + Kiwi_ATOM_READ_U32(&pSHM_HDR->commitId); /* Point to newest position has written */
		pSHM->sequence = 0;
		/* Preallocate buffers, avoid calling malloc()/free() many times */
		if (wantedSize > 0) {
			pSHM->bufferPreallocateSize = wantedSize;
			pSHM->bufferPreallocatePtr = (uint8_t*)malloc(wantedSize * sizeof(uint8_t));
			if (!pSHM->bufferPreallocatePtr) {
				free(pSHM);
				return NULL;
			}
		}
	}
	
	return (void*)pSHM;
}

/**
 * @brief Delete a Kiwi Ring Buffer shared memory context.
 * @param [in] me A pointer to the RB_SHM_CONTEXT_S structure.
 * @return 0 on success, -1 on invalid parameter, -2 on error.
 * @note The created shared memory context is not valid until Kiwi_RingBuffer_Delete is called.
 */
int Kiwi_RingBuffer_Delete(KIWI_RB_MEDIA_HANDLE_T me) {
	if (!me) {
		return -1;
	}
	RB_SHM_CONTEXT_S *pSHM = (RB_SHM_CONTEXT_S*)me;
	RB_SHM_HDR_S *pSHM_HDR = (RB_SHM_HDR_S*)pSHM->addrParentPtr;
	if (pSHM->bufferPreallocatePtr) {
		free(pSHM->bufferPreallocatePtr);
	}

	#if KIWI_SYNCHRONOUS_ENABLE
	if (pSHM->role == KIWI_RB_MEDIA_PRODUCER) {
		pthread_cond_destroy(&pSHM_HDR->signaling);
		pthread_mutex_destroy(&pSHM_HDR->semaphor);
	}
	#endif

	Kiwi_SHM_Close(pSHM->sId, pSHM->addrParentPtr, (pSHM->role == KIWI_RB_MEDIA_PRODUCER) ? true : false);
	free(pSHM);
	return 0;
}

/**
 * @brief Send a frame to a Kiwi Ring Buffer.
 * @param [in] me A pointer to the RB_SHM_CONTEXT_S structure.
 * @param [in] Frame A pointer to the KIWI_RB_MEDIA_FRAMED_S structure.
 * @return 0 on success, -1 on invalid parameter, -2 on error.
 * @note This function is only valid for the producer role.
 */
int Kiwi_RingBuffer_SendTo(KIWI_RB_MEDIA_HANDLE_T me, KIWI_RB_MEDIA_FRAMED_S *Frame) {
	if (!me || !Frame) {
		return -1;
	}

	uint32_t remain = 0;
	uint32_t totalBytes = 0;
	RB_SHM_CONTEXT_S *pSHM = (RB_SHM_CONTEXT_S*)me;
	RB_SHM_HDR_S *pSHM_HDR = (RB_SHM_HDR_S*)pSHM->addrParentPtr;

	if (pSHM->role != KIWI_RB_MEDIA_PRODUCER) {
		return -1;
	}
	if (Frame->dataLen > (pSHM->memSizeUsable + sizeof(RB_SHM_CONTEXT_S))) {
		printf("Data exceed ring buffer size (%u > %u)\r\n", Frame->dataLen, (pSHM->memSizeUsable + sizeof(RB_SHM_CONTEXT_S)));
		return -2;
	}
	remain = pSHM->addrOfTailPtr - pSHM->addrOfRuntimePtr;
	/* Point to begin position if size remain is not enough */
	if (remain < sizeof(RB_FRAMER_HDR_S)) {
		pSHM->addrOfRuntimePtr = pSHM->addrOfHeadPtr;
	}
	/* Auto increase sequence to mark next frame sequence */
	uint32_t runtimeSequence = ++(pSHM->sequence);

	RB_FRAMER_HDR_S *fHdr = (RB_FRAMER_HDR_S*)pSHM->addrOfRuntimePtr;
	fHdr->id = Frame->id;
	fHdr->sequence = runtimeSequence;
	fHdr->type = Frame->type;
	fHdr->length = Frame->dataLen;
	fHdr->encoder = Frame->encoder;
	fHdr->timestamp = Frame->timestamp;
	fHdr->monotonic = Frame->monotonic;
	fHdr->magicNum = KIWI_FRAMER_HDR_MAGICNUM;
	fHdr->framePerSeconds = Frame->framePerSeconds;

	/* Seek the current position & calculate size remain */
	pSHM->addrOfRuntimePtr += sizeof(RB_FRAMER_HDR_S);
	remain = pSHM->addrOfTailPtr - pSHM->addrOfRuntimePtr;

	/* Clone data into share memory */
	if (Frame->pData && Frame->dataLen > 0) {
		totalBytes = Frame->dataLen;
		if (totalBytes > remain) {
			memcpy(pSHM->addrOfRuntimePtr, Frame->pData, remain);
			pSHM->addrOfRuntimePtr = pSHM->addrOfHeadPtr;
			totalBytes -= remain;
			memcpy(pSHM->addrOfRuntimePtr, (uint8_t*)Frame->pData + remain, totalBytes);
			pSHM->addrOfRuntimePtr += totalBytes;
		}
		else {
			memcpy(pSHM->addrOfRuntimePtr, Frame->pData, totalBytes);
			pSHM->addrOfRuntimePtr += totalBytes;
		}	
	}

	/* Update current position */
	remain = pSHM->addrOfTailPtr - pSHM->addrOfRuntimePtr;
	if (remain < sizeof(RB_FRAMER_HDR_S)) {
		pSHM->addrOfRuntimePtr = pSHM->addrOfHeadPtr;
	}
	else {
		/* 	
			IMPORTANT:
			We MUST-BE aligned 4 bytes before writing into memory because 
			some old architectures does not support unaligned access.
		*/
		int align = pSHM->addrOfRuntimePtr - pSHM->addrOfHeadPtr;
		int divider = align % KIWI_NUMBYTES_ALIGNMENT;
		if (divider != 0) {
			int padding = KIWI_NUMBYTES_ALIGNMENT - divider;
			pSHM->addrOfRuntimePtr = pSHM->addrOfHeadPtr + align + padding;
		}
	}

	/* Update frame header */	
	fHdr->nextIndexStep = ((uint8_t*)pSHM->addrOfRuntimePtr - pSHM->addrOfHeadPtr);
	fHdr->previousIndexStep = Kiwi_ATOM_READ_U32(&pSHM_HDR->runtimId);
	fHdr->uniqeNum = Kiwi_HDR_GenerateUniqueCode(fHdr);

	/* Update ring buffer header commit new index for reading */
	uint32_t runtimeIndex = (uint8_t*)fHdr - pSHM->addrOfHeadPtr;
	Kiwi_ATOM_WRITE_U32(&pSHM_HDR->runtimId, runtimeIndex);
	Kiwi_HDR_SetCommit(pSHM_HDR, runtimeIndex, runtimeSequence);
	
	/* Setup next frame header empty */
	RB_FRAMER_HDR_S *nextFrameHdrPtr = (RB_FRAMER_HDR_S*)pSHM->addrOfRuntimePtr;
	memset(nextFrameHdrPtr, 0, sizeof(RB_FRAMER_HDR_S));
	nextFrameHdrPtr->magicNum = KIWI_FRAMER_HDR_MAGICNUM;
	nextFrameHdrPtr->uniqeNum = Kiwi_HDR_GenerateUniqueCode(nextFrameHdrPtr);

	return 0;
}

/**
 * @brief Read a frame from a Kiwi Ring Buffer shared memory context.
 * @param [in] me A pointer to the RB_SHM_CONTEXT_S structure.
 * @param [in] Frame A pointer to the KIWI_RB_MEDIA_FRAMED_S structure.
 * @return 0 on success, -1 on invalid parameter, -2 on invalid magic number or uniqe number, -3 on realloc error.
 * @note The created shared memory context is not valid until Kiwi_RingBuffer_Delete is called.
 */
int Kiwi_RingBuffer_ReadFrom(KIWI_RB_MEDIA_HANDLE_T me, KIWI_RB_MEDIA_FRAMED_S *Frame) {
	if (!me || !Frame) {
		return -1;
	}

	RB_SHM_CONTEXT_S *pSHM = (RB_SHM_CONTEXT_S*)me;
	RB_SHM_HDR_S *pSHM_HDR = (RB_SHM_HDR_S*)pSHM->addrParentPtr;
	uint8_t *addrOfRuntimePtr = pSHM->addrOfRuntimePtr;
	RB_FRAMER_HDR_S *fHdr = (RB_FRAMER_HDR_S*)addrOfRuntimePtr;	

	if (pSHM->role != KIWI_RB_MEDIA_CONSUMER) {
		return -2;
	}

	if (!Kiwi_HDR_IsValidFrame(pSHM, fHdr)) {
		/* Immediately return the newest commit id */
		uint32_t runtimeId = 0;
		Kiwi_HDR_GetCommit(pSHM_HDR, &runtimeId, NULL);
		pSHM->addrOfRuntimePtr = pSHM->addrOfHeadPtr + runtimeId;
		return -3;
	}

	/* Newest frame doesn't found */
	if (fHdr->length == 0) {
		return 1;
	}

	/* Avoid we read duplicate frame if reading too fast */
	if (fHdr->sequence <= pSHM->sequence) {
		pSHM->addrOfRuntimePtr = pSHM->addrOfHeadPtr + fHdr->nextIndexStep;
		fHdr = (RB_FRAMER_HDR_S*)pSHM->addrOfRuntimePtr;
		return -4;
	}

	/* We can reallocate buffer if suddenly frame bigger than preallocate buffers */
	if (fHdr->length > pSHM->bufferPreallocateSize) {
		printf("Reallocate SHM %d memory size from %d to %d\r\n", pSHM->sId, pSHM->bufferPreallocateSize, fHdr->length);
		void *newPtr = realloc(pSHM->bufferPreallocatePtr, fHdr->length);
		if (!newPtr) {
			return -5;
		}
		pSHM->bufferPreallocatePtr = (uint8_t*)newPtr;
		pSHM->bufferPreallocateSize = fHdr->length;
	}
	Frame->id = fHdr->id;
	Frame->type = fHdr->type;
	Frame->pData = pSHM->bufferPreallocatePtr;
	Frame->dataLen = fHdr->length;
	Frame->encoder = fHdr->encoder;
	Frame->timestamp = fHdr->timestamp;
	Frame->monotonic = fHdr->monotonic;
	Frame->framePerSeconds = fHdr->framePerSeconds;

	/* Seek pointer to bytes data */
	addrOfRuntimePtr += sizeof(RB_FRAMER_HDR_S);
	uint32_t readBytes = fHdr->length;
	uint32_t remain = pSHM->addrOfTailPtr - addrOfRuntimePtr;
	if (readBytes > remain) {
		memcpy(Frame->pData, addrOfRuntimePtr, remain);
		addrOfRuntimePtr = pSHM->addrOfHeadPtr;
		readBytes -= remain;
		memcpy((uint8_t*)Frame->pData + remain, addrOfRuntimePtr, readBytes);
		addrOfRuntimePtr += readBytes;
	}
	else {
		memcpy(Frame->pData, addrOfRuntimePtr, readBytes);
		addrOfRuntimePtr += readBytes;
	}

	pSHM->sequence = fHdr->sequence;
	pSHM->addrOfRuntimePtr = pSHM->addrOfHeadPtr + fHdr->nextIndexStep;

	return 0;
}

/**
 * @brief Wait for data in ring buffer (blocking)
 * @param me Ring buffer handle
 * @return 0 on success, -1 if invalid handle, -2 if not consumer
 */
int Kiwi_RingBuffer_Wait(KIWI_RB_MEDIA_HANDLE_T me) {
	if (!me) {
		return -1;
	}
	RB_SHM_CONTEXT_S *pSHM = (RB_SHM_CONTEXT_S*)me;
	RB_SHM_HDR_S *pSHM_HDR = (RB_SHM_HDR_S*)pSHM->addrParentPtr;

	if (pSHM->role != KIWI_RB_MEDIA_CONSUMER) {
		return -2;
	}

	/* Wait for data available and commit index updated */
	#if KIWI_SYNCHRONOUS_ENABLE
	pthread_mutex_lock(&pSHM_HDR->semaphor);
	pthread_cond_wait(&pSHM_HDR->signaling, &pSHM_HDR->semaphor);
	pthread_mutex_unlock(&pSHM_HDR->semaphor);
	#endif

	return 0;
}

/**
 * @brief Seek to the newest frame that has a timestamp less than or equal to given timestamp.
 * @param [in] me A pointer to the RB_SHM_CONTEXT_S structure.
 * @param [in] timestamp The timestamp to seek to.
 * @return 0 on success, -1 on invalid parameter, -2 on error, -3 on failed to find a frame.
 * @note The created shared memory context is not valid until Kiwi_RingBuffer_Delete is called.
 */
int Kiwi_RingBuffer_SeekTo(KIWI_RB_MEDIA_HANDLE_T me, uint64_t timestamp) {
	if (!me) {
		return -1;
	}

	uint8_t *addr = NULL;
	bool hasFound = false;
	uint32_t runtimId = 0;
	RB_FRAMER_HDR_S *fHdr = NULL;
	RB_SHM_CONTEXT_S *pSHM = NULL;

	pSHM = (RB_SHM_CONTEXT_S*)me;
	Kiwi_HDR_GetCommit(pSHM->addrParentPtr, &runtimId, NULL);
	addr = pSHM->addrOfHeadPtr + runtimId;
	fHdr = (RB_FRAMER_HDR_S*)addr;

	if (fHdr->magicNum != KIWI_FRAMER_HDR_MAGICNUM || fHdr->uniqeNum != Kiwi_HDR_GenerateUniqueCode(fHdr))  {
		return -2;
	}

	while (1) {
		uint64_t ts = fHdr->timestamp;

		if (fHdr->encoder == KIWI_RB_MEDIA_ENCODER_FRAME_H264 || fHdr->encoder == KIWI_RB_MEDIA_ENCODER_FRAME_H265) {
			if (fHdr->type == KIWI_RB_MEDIA_FRAME_HDR_TYPE_I && ts <= timestamp) {
				hasFound = true;
				break;
			}
		}
		else { 
			if (ts <= timestamp) {
				hasFound = true;
				break;
			}
		}

		runtimId = fHdr->previousIndexStep;
		addr = pSHM->addrOfHeadPtr + runtimId;
		RB_FRAMER_HDR_S *fHdrPrevious = (RB_FRAMER_HDR_S *)addr;
		if (fHdrPrevious->magicNum != KIWI_FRAMER_HDR_MAGICNUM || 
			fHdrPrevious->uniqeNum != Kiwi_HDR_GenerateUniqueCode(fHdrPrevious))  {
			break;
		}
		fHdr = fHdrPrevious;
	}

	if (hasFound) {
		pSHM->sequence = 0;
		pSHM->addrOfRuntimePtr = (uint8_t*)fHdr;
		return 0;
	}
	return -3;
}

/**
 * @brief Opens a shared memory block.
 * @param [out] sId A pointer to the integer that will contain the shared memory block id.
 * @param [in] uri The uri of the shared memory block.
 * @param [in] wantedSize The minimum size of the shared memory block.
 * @param [in] readOnly true to open the shared memory block as read-only, false to open it as read-write.
 * @return A pointer to the shared memory block.
 * @note The created shared memory context is not valid until shmdt() is called.
 */
void * Kiwi_SHM_Open(int *sId, const char *uri, uint32_t wantedSize, bool readOnly) {
	int id;
	key_t k;
	void *addrParentPtr = NULL;

	if (!readOnly) {
		if (access(uri, F_OK) != 0) {
			FILE *fp = fopen(uri, "w+");
			if (fp) {
				fclose(fp);
			}
		}
	}

	k = ftok(uri, 0);
	if (k < 0) {
		printf("ftok()\r\n");
		return NULL;
	}

	id = (readOnly ? shmget(k, 0, 0666) : shmget(k, wantedSize, IPC_CREAT | 0666));
	if (id < 0) {
		printf("shmget()\r\n");
		return NULL;
	}

	addrParentPtr = shmat(id, NULL, 0);
	if (addrParentPtr == ((void *)-1)) {
		printf("shmat()\r\n");
		return NULL;
	}
	*sId = id;

	return addrParentPtr;
}

/**
 * @brief Close a shared memory context.
 * @param [in] sId The shared memory segment ID, returned by Kiwi_SHM_Open.
 * @param [in] pSHM The shared memory pointer, returned by Kiwi_SHM_Open.
 * @param [in] erase true to delete the shared memory segment after detach, false to simply detach the shared memory segment.
 * @return None.
 * @note The created shared memory context is not valid until Kiwi_SHM_Close is called.
 */
void Kiwi_SHM_Close(int sId, void *pSHM, bool erase) {
	if (pSHM) {
		shmdt(pSHM);
	}
	if (erase && sId >= 0) {
		shmctl(sId, IPC_RMID, NULL);
	}
}

/**
 * @brief Sets the commit index and sequence of a frame header.
 * @param [in] pSHM_HDR A pointer to the RB_SHM_HDR_S structure.
 * @param [in] u32Index The commit index to set.
 * @param [in] u32Sequence The sequence to set.
 * @return None.
 * @note The commit index is used to track the order in which frames were written to the ring buffer.
 */
inline void Kiwi_HDR_SetCommit(RB_SHM_HDR_S *pSHM_HDR, uint32_t u32Index, uint32_t u32Sequence) {
	Kiwi_ATOM_WRITE_U32(&pSHM_HDR->commitId, u32Index);
	Kiwi_ATOM_WRITE_U32(&pSHM_HDR->sequence, u32Sequence);

	/* BROADCAST to all consumers that new data is available */
	#if KIWI_SYNCHRONOUS_ENABLE
	int rc = pthread_mutex_lock(&pSHM_HDR->semaphor);
	if (rc == EOWNERDEAD) {
        /**
		 * Mark the mutex as consistent after the previous owner died and
		 * repair the robust mutex state
		 */
        pthread_mutex_consistent(&pSHM_HDR->semaphor);
    }
	pthread_cond_broadcast(&pSHM_HDR->signaling);
	pthread_mutex_unlock(&pSHM_HDR->semaphor);
	#endif
}

/**
 * @brief Generates a unique code from a frame header.
 * @param[in] fHdr The frame header to generate a unique code from.
 * @return A unique code generated from the frame header.
 * @note The unique code is generated by XORing the following variables:
 * - id
 * - nextIndexStep
 * - previousIndexStep
 * - length
 * - magicNum
 * - Two 32-bit parts of the timestamp
 * - Two 32-bit parts of the monotonic
 */
inline uint32_t Kiwi_HDR_GenerateUniqueCode(RB_FRAMER_HDR_S *fHdr) {
	assert(fHdr);
	
	uint32_t xor = 0;
    xor ^= fHdr->id;
	xor ^= fHdr->sequence;
	xor ^= fHdr->nextIndexStep;
	xor ^= fHdr->previousIndexStep;
    xor ^= fHdr->length;
	xor ^= fHdr->magicNum;
    /* Split 64-bit timestamp into two 32-bit parts */
    xor ^= (uint32_t)(fHdr->timestamp & 0xFFFFFFFF);
    xor ^= (uint32_t)((fHdr->timestamp >> 32) & 0xFFFFFFFF);
	xor ^= (uint32_t)(fHdr->monotonic & 0xFFFFFFFF);
    xor ^= (uint32_t)((fHdr->monotonic >> 32) & 0xFFFFFFFF);
	
    return xor;
}

/**
 * @brief Get the index and sequence of the last committed frame.
 * @param [in] pSHM_HDR A pointer to the RB_SHM_HDR_S structure.
 * @param [out] u32Index The index of the last committed frame.
 * @param [out] u32Sequence The sequence of the last committed frame.
 * @return None.
 * @note The created shared memory context is not valid until Kiwi_RingBuffer_Delete is called.
 */
inline void Kiwi_HDR_GetCommit(RB_SHM_HDR_S *pSHM_HDR, uint32_t *u32Index, uint32_t *u32Sequence) {
	if (u32Index) {
		*u32Index = Kiwi_ATOM_READ_U32(&pSHM_HDR->commitId);
	}
	if (u32Sequence) {
		*u32Sequence = Kiwi_ATOM_READ_U32(&pSHM_HDR->sequence);
	}
}

/**
 * @brief Check if a frame is valid.
 * @param[in] pSHM The shared memory context.
 * @param[in] fHdr The frame header to check.
 * @return true if the frame is valid, false otherwise.
 * @note A frame is considered valid if all the following conditions are met:
 * - The shared memory context is not null.
 * - The frame header is not null.
 * - The frame header's magic number matches KIWI_FRAMER_HDR_MAGICNUM.
 * - The frame header's unique number matches the result of Kiwi_HDR_GenerateUniqueCode.
 * - The frame header's length is less than or equal to the usable memory size minus the size of the frame header.
 * - The frame header's next index step is less than the usable memory size.
 * - The frame header's previous index step is less than the usable memory size.
 */
static inline bool Kiwi_HDR_IsValidFrame(RB_SHM_CONTEXT_S *pSHM, RB_FRAMER_HDR_S *fHdr) {
	if (!pSHM || !fHdr) {
        return false;
    }

    if (fHdr->magicNum != KIWI_FRAMER_HDR_MAGICNUM) {
        return false;
    }
    if (fHdr->uniqeNum != Kiwi_HDR_GenerateUniqueCode(fHdr)) {
        return false;
    }
    if (fHdr->length > (pSHM->memSizeUsable - sizeof(RB_FRAMER_HDR_S))) {
        return false;
    }
    return true;
}

/*-----------------------------------------------------------------------------------*/

/**
 * @brief Atomically read a 32-bit unsigned integer.
 * @param [in] u32ValuePtr Pointer to the value to read.
 * @return The current value stored at u32ValuePtr.
 * @note This operation is thread-safe and process-safe when the value
 *       resides in shared memory. It uses sequential consistency
 *       (__ATOMIC_SEQ_CST), providing the strongest memory ordering.
 */
uint32_t Kiwi_ATOM_READ_U32(uint32_t *u32ValuePtr) {
	return __atomic_load_n(u32ValuePtr, __ATOMIC_SEQ_CST);
}

/**
 * @brief Atomically write a 32-bit unsigned integer.
 * @param [out] u32ValuePtr Pointer to the value to update.
 * @param [in] u32Value New value to store.
 * @return None.
 * @note This operation is thread-safe and process-safe when the value
 *       resides in shared memory. It uses sequential consistency
 *       (__ATOMIC_SEQ_CST), ensuring all readers observe writes in a
 *       well-defined order.
 */
void Kiwi_ATOM_WRITE_U32(uint32_t *u32ValuePtr, uint32_t u32Value) {
	__atomic_store_n(u32ValuePtr, u32Value, __ATOMIC_SEQ_CST);
}

/*-----------------------------------------------------------------------------------*/