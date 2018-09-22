#ifndef DARKNET_WRAPPER_HPP
#define DARKNET_WRAPPER_HPP

#include <grpcpp/grpcpp.h>
#include <grpc/support/log.h>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <sys/time.h>

extern "C" {
	#undef __cplusplus
	#include "darknet.h"
	#define __cplusplus 1
}

struct timestamp {
    struct timeval start;
    struct timeval end;
};

static inline void tvsub(struct timeval *x,
						 struct timeval *y,
						 struct timeval *ret)
{
	ret->tv_sec = x->tv_sec - y->tv_sec;
	ret->tv_usec = x->tv_usec - y->tv_usec;
	if (ret->tv_usec < 0) {
		ret->tv_sec--;
		ret->tv_usec += 1000000;
	}
}

void probe_time_start2(struct timestamp *ts)
{
    gettimeofday(&ts->start, NULL);
}

float probe_time_end2(struct timestamp *ts)
{
    struct timeval tv;
    gettimeofday(&ts->end, NULL);
	tvsub(&ts->end, &ts->start, &tv);
	return (tv.tv_sec * 1000.0 + (float) tv.tv_usec / 1000.0);
}

namespace DarknetWrapper {

	typedef struct
	{
		bool done;
		const darknetServer::KeyFrame *frame;
		detection *dets;
		int nboxes;
		int classes;
		void *tag;
	} WorkRequest;

	class DetectionQueue
	{
	public:

		void push_back(WorkRequest elem) {
			std::lock_guard<std::mutex> lock(this->mutex);
			this->queue.push(elem);
			this->cv.notify_one();
		}

		// pops 1 element
		void pop_front(WorkRequest &elems) {
			std::unique_lock<std::mutex> lock(this->mutex);
			if(this->queue.empty())
				cv.wait(lock, [this](){ return !this->queue.empty(); });

			// Once the cv wakes us up....
			if(!this->queue.empty()) {
				elem = (this->queue.front());
				this->queue.pop();
			}
		}

		// Pops upto N elements
		void pop_front(std::vector<WorkRequest> &elems, int numElems) {
			std::unique_lock<std::mutex> lock(this->mutex);
			if(this->queue.empty())
				cv.wait(lock, [this](){ return !this->queue.empty(); });

			// Once the cv wakes us up....
			int numPopped = 0;
			while( !this->queue.empty() && (numPopped < numElems) ) {
				elems.emplace_back(this->queue.front());
				this->queue.pop();
				numPopped++;
			}
		}

	private:
		std::queue<WorkRequest> queue;
		std::mutex mutex;
		std::condition_variable cv;

	}; // class DetectionQueue

	class Detector {
	public:

		void Init(int argc, char** argv) {
			// Initialization: Load config files, labels, graph, etc.,
			// Config the GPU and get into a thread that is ready to accept
			// images for detection.
			#ifdef GPU
			cuda_set_device(0);
			#endif
			char *datacfg = argv[1];
			char *cfgfile = argv[2];
			char *weightfile = argv[3];

			this->net = load_network(cfgfile, weightfile, 0);
			this->gpuBufferInit = false;

			this->numNetworkOutputs = this->size_network();
			this->predictions = new float[numNetworkOutputs];
			this->average = new float[numNetworkOutputs];
		}

		void Shutdown() {
			// Free any darknet resources held. Close the GPU connection, etc...
			delete this->predictions;
			delete this->average;
			free_network(this->net);
		}

		void doDetection(WorkRequest &elem) {
			float nms = .4;
			set_batch_network(net, 1);
			layer l = net->layers[net->n-1];

			image newImage;
			image newImage_letterboxed;

			std::cout << "doDetection: new requeust " << elem.tag << std::endl;
			probe_time_start2(&ts_detect);

			// Convert to the right format
			// Allocate memory for data in 'image', based on the size of 'data' in frame
			newImage.data = new float[elem.frame->data()->size()];

			// Copy from the frame in elem to the 'image' format that darknet uses internally...
			this->convertFrameToImage(elem.frame, &newImage);

			//save_image(newImage, "recieved");

			// Convert to the RGBGR format that YOLO operates on..
			rgbgr_image(newImage);

			// Add black borders (letter-boxing) around the image to ensure that the image
			// is of the correct width and height that YOLO expects.
			newImage_letterboxed = letterbox_image(newImage, net->w, net->h);

			//save_image(newImage_letterboxed, "letterboxed");

			/* Now we finally run the actual network	*/
			probe_time_start2(&ts_gpu);

/*			// This block is used to test NoTransfer and NoGPUCompute
			bool transferData = false;
			if (gpuBufferInit == false) {
				transferData = true;
				gpuBufferInit = true;
//				network_predict2(net, newImage_letterboxed.data, transferData);
//				this->remember_network();
//				this->dets = this->average_predictions(&(this->nboxes), newImage.h, newImage.w);
			}
			network_predict2(net, newImage_letterboxed.data, transferData);
			this->remember_network();
			elem.dets = this->average_predictions(&(elem.nboxes), newImage.h, newImage.w);
//			elem.dets = this->dets;
//			elem.nboxes = this->nboxes;
*/
			network_predict(net, newImage_letterboxed.data);
			elem.dets = get_network_boxes(this->net, newImage.w, newImage.h, 0.5, 0.5, 0, 1, &(elem.nboxes));

			// What the hell does this do?
			if (nms > 0) {
				do_nms_obj(elem.dets, elem.nboxes, l.classes, nms);
			}

			std::cout << elem.tag << " GPU processing took " << probe_time_end2(&ts_gpu) << " milliseconds"<< std::endl;

			//draw_detections(newImage_letterboxed, elem.dets, elem.nboxes, 0.5, NULL, NULL, l.classes);
			//save_image(newImage_letterboxed, "detected");
			elem.classes = l.classes;
			elem.done = true;

			std::cout << elem.tag << " doDetection: took " << probe_time_end2(&ts_detect) << " milliseconds"<< std::endl;
		}

	private:
		void convertFrameToImage(const darknetServer::KeyFrame *frame, image *newImage) {
			newImage->w = frame->width();
			newImage->h = frame->height();
			newImage->c = frame->numChannels();
			newImage->data =  const_cast<float *>(frame->data()->data());
		}

		// Helper functions stolen from demo.c
		int size_network()
		{
			int count = 0;
			for(int i = 0; i < this->net->n; ++i){
				layer l = this->net->layers[i];
				if(l.type == YOLO || l.type == REGION || l.type == DETECTION){
					count += l.outputs;
				}
			}
			return count;
		}

		// All the darknet globals.
		struct timestamp ts_detect;
		struct timestamp ts_gpu;
		float *predictions;
		float *average;
		bool gpuBufferInit;
		detection *dets;
		int nboxes;
		network *net;
		int numNetworkOutputs;

	}; // class Detector

	class AsyncDetector : Detector
	{
	  public:
		void Init(int argc, char** argv, DetectionQueue *requestQueue, DetectionQueue *completionQueue) {
			// Store pointers to the workQueues
			this->requestQueue = requestQueue;
			this->completionQueue = completionQueue;
			Detector::Init(argc, argv);
		}

		void Shutdown() {
			// Set locally owned pointers to NULL;
			this->requestQueue = nullptr;
			this->completionQueue = nullptr;
			Detector::Shutdown();
		}

		void doDetection() {
			std::vector<WorkRequest> elems;
			elem.reserver(10);
			while(true) {
				// Wait on the requestQueue
				requestQueue->pop_front(elems, 10);
				Detector::doDetection(elems);
				// Put the result back on the completionQueue.
				for (auto elemIterator = elems.begin(); elemIterator != elems.end(); ++elemIterator)
					completionQueue->push_back(*elemIterator);
				elems.clear();
			}
		}

	  private:
		DetectionQueue *requestQueue;
		DetectionQueue *completionQueue;
	};

} // namespace DarknetWrapper

#endif // DARKNET_WRAPPER_CPP
