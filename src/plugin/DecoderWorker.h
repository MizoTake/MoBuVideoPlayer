#pragma once

#include "mobu/media/Decoder.h"

#include <condition_variable>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>

namespace mobu::plugin {

class DecoderWorker final {
public:
    explicit DecoderWorker(std::filesystem::path path)
        : thread_([this, path = std::move(path)] { Run(path); }) {}

    ~DecoderWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    DecoderWorker(const DecoderWorker&) = delete;
    DecoderWorker& operator=(const DecoderWorker&) = delete;

    struct Result {
        media::VideoInfo info;
        media::VideoFrame frame;
    };

    Result DecodeAt(std::int64_t time100ns) {
        Work work;
        work.time100ns = time100ns;
        auto result = work.done.get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                throw std::runtime_error("Native video decoder is stopping.");
            }
            queue_.push(std::move(work));
        }
        wake_.notify_one();
        return result.get();
    }

    media::FrameRange ResolveFrameRange(std::int64_t startFrame, std::int64_t stopFrame) {
        Work work;
        work.resolveRange = true;
        work.startFrame = startFrame;
        work.stopFrame = stopFrame;
        auto result = work.rangeDone.get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                throw std::runtime_error("Native video decoder is stopping.");
            }
            queue_.push(std::move(work));
        }
        wake_.notify_one();
        return result.get();
    }

private:
    struct Work {
        std::int64_t time100ns = 0;
        std::int64_t startFrame = 0;
        std::int64_t stopFrame = -1;
        bool resolveRange = false;
        std::promise<Result> done;
        std::promise<media::FrameRange> rangeDone;
    };

    void Run(const std::filesystem::path& path) noexcept {
        std::unique_ptr<media::IDecoder> decoder;
        std::exception_ptr failure;
        try {
            decoder = media::CreateNativeDecoder();
            decoder->Open(path);
        } catch (...) {
            failure = std::current_exception();
        }
        for (;;) {
            Work work;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (stopping_ && queue_.empty()) {
                    break;
                }
                work = std::move(queue_.front());
                queue_.pop();
            }
            try {
                if (failure) {
                    std::rethrow_exception(failure);
                }
                if (work.resolveRange) {
                    work.rangeDone.set_value(decoder->ResolveFrameRange(work.startFrame, work.stopFrame));
                } else {
                    Result result;
                    result.frame = decoder->DecodeAt(work.time100ns);
                    result.info = decoder->Info();
                    work.done.set_value(std::move(result));
                }
            } catch (...) {
                failure = std::current_exception();
                if (work.resolveRange) {
                    work.rangeDone.set_exception(failure);
                } else {
                    work.done.set_exception(failure);
                }
            }
        }
        // COM-backed decoders must be released on the same thread that opened them.
        decoder.reset();
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::queue<Work> queue_;
    bool stopping_ = false;
    std::thread thread_;
};

}
