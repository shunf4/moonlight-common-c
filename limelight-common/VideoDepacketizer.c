#include "Platform.h"
#include "Limelight-internal.h"
#include "LinkedBlockingQueue.h"
#include "Video.h"

static PLENTRY nalChainHead;
static int nalChainDataLength;

static int nextFrameNumber = 1;
static int nextPacketNumber;
static int startFrameNumber = 1;
static int waitingForNextSuccessfulFrame;
static int waitingForIdrFrame = 1;
static int gotNextFrameStart;
static int lastPacketInStream = 0;
static int decodingFrame = 0;

static LINKED_BLOCKING_QUEUE decodeUnitQueue;
static unsigned int nominalPacketDataLength;

static unsigned short lastSequenceNumber;

typedef struct _BUFFER_DESC {
	char* data;
	unsigned int offset;
	unsigned int length;
} BUFFER_DESC, *PBUFFER_DESC;

void initializeVideoDepacketizer(int pktSize) {
	LbqInitializeLinkedBlockingQueue(&decodeUnitQueue, 15);
	nominalPacketDataLength = pktSize - sizeof(NV_VIDEO_PACKET);
}

static void cleanupAvcFrameState(void) {
	PLENTRY lastEntry;

	while (nalChainHead != NULL) {
		lastEntry = nalChainHead;
		nalChainHead = lastEntry->next;
		free(lastEntry->data);
		free(lastEntry);
	}

	nalChainDataLength = 0;
}

static void dropAvcFrameState(void) {
	waitingForIdrFrame = 1;
	cleanupAvcFrameState();
}

void destroyVideoDepacketizer(void) {
	PLINKED_BLOCKING_QUEUE_ENTRY entry, nextEntry;
	
	entry = LbqDestroyLinkedBlockingQueue(&decodeUnitQueue);
	while (entry != NULL) {
		nextEntry = entry->flink;
		free(entry->data);
		free(entry);
		entry = nextEntry;
	}

	cleanupAvcFrameState();
}

static int isSeqFrameStart(PBUFFER_DESC candidate) {
	return (candidate->length == 4 && candidate->data[candidate->offset + candidate->length - 1] == 1);
}

static int isSeqAvcStart(PBUFFER_DESC candidate) {
	return (candidate->data[candidate->offset + candidate->length - 1] == 1);
}

static int isSeqPadding(PBUFFER_DESC candidate) {
	return (candidate->data[candidate->offset + candidate->length - 1] == 0);
}

static int getSpecialSeq(PBUFFER_DESC current, PBUFFER_DESC candidate) {
	if (current->length < 3) {
		return 0;
	}

	if (current->data[current->offset] == 0 &&
		current->data[current->offset + 1] == 0) {
		// Padding or frame start
		if (current->data[current->offset + 2] == 0) {
			if (current->length >= 4 && current->data[current->offset + 3] == 1) {
				// Frame start
				candidate->data = current->data;
				candidate->offset = current->offset;
				candidate->length = 4;
				return 1;
			}
			else {
				// Padding
				candidate->data = current->data;
				candidate->offset = current->offset;
				candidate->length = 3;
				return 1;
			}
		}
		else if (current->data[current->offset + 2] == 1) {
			// NAL start
			candidate->data = current->data;
			candidate->offset = current->offset;
			candidate->length = 3;
			return 1;
		}
	}

	return 0;
}

static void reassembleAvcFrame(int frameNumber) {
	if (nalChainHead != NULL) {
		PDECODE_UNIT du = (PDECODE_UNIT) malloc(sizeof(*du));
		if (du != NULL) {
			du->bufferList = nalChainHead;
			du->fullLength = nalChainDataLength;

			nalChainHead = NULL;
			nalChainDataLength = 0;

			if (LbqOfferQueueItem(&decodeUnitQueue, du) == LBQ_BOUND_EXCEEDED) {
				Limelog("Decode unit queue overflow\n");

				nalChainHead = du->bufferList;
				nalChainDataLength = du->fullLength;
				free(du);

				// FIXME: Get proper lower bound
				connectionSinkTooSlow(0, frameNumber);

				// Clear frame state and wait for an IDR
				dropAvcFrameState();
				return;
			}

			// Notify the control connection
			connectionReceivedFrame(frameNumber);
		}
	}
}

int getNextDecodeUnit(PDECODE_UNIT *du) {
	int err = LbqWaitForQueueElement(&decodeUnitQueue, (void**)du);
	if (err == LBQ_SUCCESS) {
		return 1;
	}
	else {
		return 0;
	}
}

void freeDecodeUnit(PDECODE_UNIT decodeUnit) {
	PLENTRY lastEntry;

	while (decodeUnit->bufferList != NULL) {
		lastEntry = decodeUnit->bufferList;
		decodeUnit->bufferList = lastEntry->next;
		free(lastEntry->data);
		free(lastEntry);
	}

	free(decodeUnit);
}

static void queueFragment(char *data, int offset, int length) {
	PLENTRY entry = (PLENTRY) malloc(sizeof(*entry));
	if (entry != NULL) {
		entry->next = NULL;
		entry->length = length;
		entry->data = (char*) malloc(entry->length);
		if (entry->data == NULL) {
			free(entry);
			return;
		}

		memcpy(entry->data, &data[offset], entry->length);

		nalChainDataLength += entry->length;

		if (nalChainHead == NULL) {
			nalChainHead = entry;
		}
		else {
			PLENTRY currentEntry = nalChainHead;

			while (currentEntry->next != NULL) {
				currentEntry = currentEntry->next;
			}

			currentEntry->next = entry;
		}
	}
}

static void processRtpPayloadSlow(PNV_VIDEO_PACKET videoPacket, PBUFFER_DESC currentPos) {
	BUFFER_DESC specialSeq;
	int decodingAvc = 0;

	while (currentPos->length != 0) {
		int start = currentPos->offset;

		if (getSpecialSeq(currentPos, &specialSeq)) {
			if (isSeqAvcStart(&specialSeq)) {
				// Now we're decoding AVC
				decodingAvc = 1;

				if (isSeqFrameStart(&specialSeq)) {
					// Now we're working on a frame
					decodingFrame = 1;

					// Reassemble any pending frame
					reassembleAvcFrame(videoPacket->frameIndex);

					if (specialSeq.data[specialSeq.offset + specialSeq.length] == 0x65) {
						// This is the NALU code for I-frame data
						waitingForIdrFrame = 0;
					}
				}

				// Skip the start sequence
				currentPos->length -= specialSeq.length;
				currentPos->offset += specialSeq.length;
			}
			else {
				// Check if this is padding after a full AVC frame
				if (decodingAvc && isSeqPadding(currentPos)) {
					reassembleAvcFrame(videoPacket->frameIndex);
				}

				// Not decoding AVC
				decodingAvc = 0;

				// Just skip this byte
				currentPos->length--;
				currentPos->offset++;
			}
		}

		// Move to the next special sequence
		while (currentPos->length != 0) {
			// Check if this should end the current NAL
			if (getSpecialSeq(currentPos, &specialSeq)) {
				if (decodingAvc || !isSeqPadding(&specialSeq)) {
					break;
				}
			}

			// This byte is part of the NAL data
			currentPos->offset++;
			currentPos->length--;
		}

		if (decodingAvc) {
			queueFragment(currentPos->data, start, currentPos->offset - start);
		}
	}
}

static int isFirstPacket(char flags) {
	// Clear the picture data flag
	flags &= ~FLAG_CONTAINS_PIC_DATA;
	
	// Check if it's just the start or both start and end of a frame
	return (flags == (FLAG_SOF | FLAG_EOF) ||
		flags == FLAG_SOF);
}

static void processRtpPayloadFast(PNV_VIDEO_PACKET videoPacket, BUFFER_DESC location) {
	queueFragment(location.data, location.offset, location.length);
}

static int isBeforeSigned(int numA, int numB, int ambiguousCase) {
	// This should be the common case for most callers
	if (numA == numB) {
		return 0;
	}

	// If numA and numB have the same signs,
	// we can just do a regular comparison.
	if ((numA < 0 && numB < 0) || (numA >= 0 && numB >= 0)) {
		return numA < numB;
	}
	else {
		// The sign switch is ambiguous
		return ambiguousCase;
	}
}

void processRtpPayload(PNV_VIDEO_PACKET videoPacket, int length) {
	BUFFER_DESC currentPos, specialSeq;
	int frameIndex;
	char flags;
	int firstPacket;

	// Mask the top 8 bits from the SPI
	videoPacket->streamPacketIndex >>= 8;
	videoPacket->streamPacketIndex &= 0xFFFFFF;
	
	currentPos.data = (char*) (videoPacket + 1);
	currentPos.offset = 0;
	currentPos.length = length - sizeof(*videoPacket);

	frameIndex = videoPacket->frameIndex;
	flags = videoPacket->flags;
	firstPacket = isFirstPacket(flags);

	// Drop duplicates or re-ordered packets
	int streamPacketIndex = videoPacket->streamPacketIndex;
	if (isBeforeSigned((short) streamPacketIndex, (short) (lastPacketInStream + 1), 0)) {
		return;
	}

	// Drop packets from a previously completed frame
	if (isBeforeSigned(frameIndex, nextFrameNumber, 0)) {
		return;
	}

	// Look for a frame start before receiving a frame end
	if (firstPacket && decodingFrame)
	{
		Limelog("Network dropped end of a frame\n");
		nextFrameNumber = frameIndex;

		// Unexpected start of next frame before terminating the last
		waitingForNextSuccessfulFrame = 1;
		waitingForIdrFrame = 1;

		// Clear the old state and wait for an IDR
		dropAvcFrameState();
	}
	// Look for a non-frame start before a frame start
	else if (!firstPacket && !decodingFrame) {
		// Check if this looks like a real frame
		if (flags == FLAG_CONTAINS_PIC_DATA ||
			flags == FLAG_EOF ||
			currentPos.length < nominalPacketDataLength)
		{
			Limelog("Network dropped beginning of a frame");
			nextFrameNumber = frameIndex + 1;

			waitingForNextSuccessfulFrame = 1;

			dropAvcFrameState();
			decodingFrame = 0;
			return;
		}
		else {
			// FEC data
			return;
		}
	}
	// Check sequencing of this frame to ensure we didn't
	// miss one in between
	else if (firstPacket) {
		// Make sure this is the next consecutive frame
		if (isBeforeSigned(nextFrameNumber, frameIndex, 1)) {
			Limelog("Network dropped an entire frame");
			nextFrameNumber = frameIndex;

			// Wait until an IDR frame comes
			waitingForNextSuccessfulFrame = 1;
			dropAvcFrameState();
		}
		else if (nextFrameNumber != frameIndex) {
			// Duplicate packet or FEC dup
			decodingFrame = 0;
			return;
		}

		// We're now decoding a frame
		decodingFrame = 1;
	}

	// If it's not the first packet of a frame
	// we need to drop it if the stream packet index
	// doesn't match
	if (!firstPacket && decodingFrame) {
		if (streamPacketIndex != (int) (lastPacketInStream + 1)) {
			Limelog("Network dropped middle of a frame");
			nextFrameNumber = frameIndex + 1;

			waitingForNextSuccessfulFrame = 1;

			dropAvcFrameState();
			decodingFrame = 0;

			return;
		}
	}

	// Notify the server of any packet losses
	if (streamPacketIndex != (int) (lastPacketInStream + 1)) {
		// Packets were lost so report this to the server
		connectionLostPackets(lastPacketInStream, streamPacketIndex);
	}
	lastPacketInStream = streamPacketIndex;

	if (isFirstPacket &&
		getSpecialSeq(&currentPos, &specialSeq) &&
		isSeqFrameStart(&specialSeq) &&
		specialSeq.data[specialSeq.offset + specialSeq.length] == 0x67)
	{
		// SPS and PPS prefix is padded between NALs, so we must decode it with the slow path
		processRtpPayloadSlow(videoPacket, &currentPos);
	}
	else
	{
		processRtpPayloadFast(videoPacket, currentPos);
	}

	if (flags & FLAG_EOF) {
		// Move on to the next frame
		decodingFrame = 0;
		nextFrameNumber = frameIndex + 1;

		// If waiting for next successful frame and we got here
		// with an end flag, we can send a message to the server
		if (waitingForNextSuccessfulFrame) {
			// This is the next successful frame after a loss event
			connectionDetectedFrameLoss(startFrameNumber, nextFrameNumber - 1);
			waitingForNextSuccessfulFrame = 0;
		}

		// If we need an IDR frame first, then drop this frame
		if (waitingForIdrFrame) {
			Limelog("Waiting for IDR frame");

			dropAvcFrameState();
			return;
		}

		reassembleAvcFrame(frameIndex);

		startFrameNumber = nextFrameNumber;
	}
}

void queueRtpPacket(PRTP_PACKET rtpPacket, int length) {
	int dataOffset;

	dataOffset = sizeof(*rtpPacket);
	if (rtpPacket->header & FLAG_EXTENSION) {
		dataOffset += 4; // 2 additional fields
	}

	processRtpPayload((PNV_VIDEO_PACKET)(((char*)rtpPacket) + dataOffset), length - dataOffset);
}
