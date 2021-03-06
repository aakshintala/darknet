#ifndef TYPES_H
#define TYPES_H

#include "../../include/darknet.h"

namespace LiveStreamDetector {

	typedef struct {
		uint8_t *data = nullptr;
		int frameSize = 0;
		uint64_t frameNum;
		Timer timer;
		void *decompressedFrameDevice = nullptr;
		bool needsCudaFree = false;
		int deviceNumDecompressed;
		int decompressedFrameSize;
		int streamNum;
		nvtxRangeId_t nvtxRangeID;
	} Frame;

	typedef struct
	{
		bool done;
		image img;
		detection *dets;
		int nboxes;
		int classes;
		int deviceNum;
		Frame *tag;
	} WorkRequest;
} //namespace

#endif //TYPES_H
