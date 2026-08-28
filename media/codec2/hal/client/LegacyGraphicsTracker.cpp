/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
// #define LOG_NDEBUG 0
#define LOG_TAG "LegacyGraphicsTracker"
#include <atomic>
#include <fcntl.h>
#include <unistd.h>

#include <gui/BufferItemConsumer.h>
#include <gui/BufferQueue.h>
#include <gui/Surface.h>
#include <media/stagefright/foundation/ADebug.h>
#include <private/android/AHardwareBufferHelpers.h>
#include <vndk/hardware_buffer.h>

#include <C2AllocatorGralloc.h>
#include <C2BlockInternal.h>
#include <codec2/aidl/LegacyGraphicsTracker.h>

namespace aidl::android::hardware::media::c2::implementation {

namespace {

static std::atomic<uint64_t> sDvTrackRender{0};
static std::atomic<uint64_t> sDvTrackNoPool{0};
static std::atomic<uint64_t> sDvTrackIgbaPool{0};
static std::atomic<uint64_t> sDvTrackMigration{0};
static std::atomic<uint64_t> sDvTrackQueued{0};
static std::atomic<uint64_t> sDvAttachRequests{0};
static std::atomic<uint64_t> sDvAttachCommits{0};
static std::atomic<uint64_t> sDvAttachReleased{0};
static std::atomic<uint64_t> sDvAttachCallbacks{0};
static std::atomic<uint64_t> sDvBufCreated{0};
static std::atomic<uint64_t> sDvBufDestroyed{0};

static constexpr int kMaxDequeueMin = 1;
static constexpr int kMaxDequeueMax = ::android::BufferQueueDefs::NUM_BUFFER_SLOTS - 2;

// Just some delay for HAL to receive the stop()/release() request.
static constexpr int kAllocateDirectDelayUs = 16666;

c2_status_t retrieveAHardwareBufferId(const C2ConstGraphicBlock& blk, uint64_t* bid) {
    std::shared_ptr<const _C2BlockPoolData> bpData = _C2BlockFactory::GetGraphicBlockPoolData(blk);
    if (!bpData || bpData->getType() != _C2BlockPoolData::TYPE_AHWBUFFER) {
        return C2_BAD_VALUE;
    }
    if (__builtin_available(android __ANDROID_API_T__, *)) {
        AHardwareBuffer* pBuf;
        if (!_C2BlockFactory::GetAHardwareBuffer(bpData, &pBuf)) {
            return C2_CORRUPTED;
        }
        int ret = AHardwareBuffer_getId(pBuf, bid);
        if (ret != ::android::OK) {
            return C2_CORRUPTED;
        }
        return C2_OK;
    } else {
        return C2_OMITTED;
    }
}

// Create a GraphicBuffer object from a graphic block.
sp<GraphicBuffer> createGraphicBuffer(const C2ConstGraphicBlock& block, uint32_t toGeneration,
                                      uint64_t toUsage) {
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint64_t usage;
    uint32_t stride;
    uint32_t generation;
    uint64_t bqId;
    int32_t bqSlot;
    ::android::_UnwrapNativeCodec2GrallocMetadata(block.handle(), &width, &height, &format, &usage,
                                                  &stride, &generation, &bqId,
                                                  reinterpret_cast<uint32_t*>(&bqSlot));

    // android::UnwrapNativCodec2GrallocHandle() returns
    // a new native_handle_t with undup'ed fds.
    native_handle_t* grallocHandle = ::android::UnwrapNativeCodec2GrallocHandle(block.handle());
    if (grallocHandle == nullptr) {
        ALOGE("DVFD UNWRAP_FAIL c2handle=%p", block.handle());
        return nullptr;
    }

    ALOGE("DVFD UNWRAP c2handle=%p gralloc=%p fds=%d ints=%d fd0=%d fd1=%d fd2=%d %ux%u fmt=%u usage=0x%llx stride=%u gen=%u bqId=%llu bqSlot=%d",
          block.handle(),
          grallocHandle,
          grallocHandle->numFds,
          grallocHandle->numInts,
          grallocHandle->numFds > 0 ? grallocHandle->data[0] : -1,
          grallocHandle->numFds > 1 ? grallocHandle->data[1] : -1,
          grallocHandle->numFds > 2 ? grallocHandle->data[2] : -1,
          width,
          height,
          format,
          (unsigned long long)usage,
          stride,
          generation,
          (unsigned long long)bqId,
          bqSlot);

    sp<GraphicBuffer> graphicBuffer =
            new GraphicBuffer(grallocHandle, GraphicBuffer::CLONE_HANDLE, width, height, format, 1,
                              (usage | toUsage), stride);
    graphicBuffer->setGenerationNumber(toGeneration);

    ALOGE("DVFD POST_GB c2handle=%p gralloc=%p gb=%p fd0=%d fd1=%d fd2=%d",
          block.handle(),
          grallocHandle,
          graphicBuffer.get(),
          grallocHandle->numFds > 0 ? grallocHandle->data[0] : -1,
          grallocHandle->numFds > 1 ? grallocHandle->data[1] : -1,
          grallocHandle->numFds > 2 ? grallocHandle->data[2] : -1);

    native_handle_delete(grallocHandle);
    return graphicBuffer;
}

}  // anonymous namespace

using ::android::BufferItemConsumer;
using ::android::BufferQueue;
using ::android::ConsumerListener;
using ::android::IConsumerListener;
using ::android::IGraphicBufferConsumer;
using ::android::IGraphicBufferProducer;
using ::android::Surface;

class LegacyGraphicsTracker::PlaceHolderSurface {
  public:
    static const int kMaxAcquiredBuffer = 2;
    // Enough number to allocate in stop/release status.
    static const int kMaxDequeuedBuffer = 16;

    explicit PlaceHolderSurface(uint64_t usage) : mUsage(usage) {}

    ~PlaceHolderSurface() {
        if (mInit == C2_NO_INIT) {
            return;
        }
        if (mSurface) {
            mSurface->disconnect(NATIVE_WINDOW_API_MEDIA);
        }
    }

    c2_status_t allocate(uint32_t width, uint32_t height, uint32_t format, uint64_t usage,
                         AHardwareBuffer** pBuf, sp<Fence>* fence) {
        std::unique_lock<std::mutex> l(mLock);
        if (mInit == C2_NO_INIT) {
            mInit = init();
        }

        if (!mBufferItemConsumer || !mSurface) {
            ALOGE("PlaceHolderSurface not properly initialized");
            return C2_CORRUPTED;
        }

        {
            native_window_set_usage(mSurface.get(), usage);
            native_window_set_buffers_format(mSurface.get(), format);
            native_window_set_buffers_dimensions(mSurface.get(), width, height);

            sp<GraphicBuffer> gb;
            ::android::status_t res = mSurface->dequeueBuffer(&gb, fence);
            if (res != ::android::OK) {
                ALOGE("dequeueBuffer failed from PlaceHolderSurface %d", res);
                return C2_CORRUPTED;
            }
            (void)mSurface->detachBuffer(gb);
            *pBuf = AHardwareBuffer_from_GraphicBuffer(gb.get());
            AHardwareBuffer_acquire(*pBuf);
        }

        return C2_OK;
    }

  private:
    uint64_t mUsage;
    sp<Surface> mSurface;
    sp<BufferItemConsumer> mBufferItemConsumer;
    c2_status_t mInit = C2_NO_INIT;
    std::mutex mLock;

    c2_status_t init() {
        std::tie(mBufferItemConsumer, mSurface) =
                BufferItemConsumer::create(mUsage, kMaxAcquiredBuffer);

        if (mSurface) {
            mSurface->connect(NATIVE_WINDOW_API_MEDIA, nullptr);
            mSurface->setMaxDequeuedBufferCount(kMaxDequeuedBuffer);
        }
        return C2_OK;
    }
};

LegacyGraphicsTracker::BufferItem::BufferItem(uint32_t generation, int slot,
                                              const sp<GraphicBuffer>& buf, const sp<Fence>& fence)
    : mInit{false}, mGeneration{generation}, mSlot{slot} {
    if (!buf) {
        return;
    }
    if (__builtin_available(android __ANDROID_API_T__, *)) {
        AHardwareBuffer* pBuf = AHardwareBuffer_from_GraphicBuffer(buf.get());
        int ret = AHardwareBuffer_getId(pBuf, &mId);
        if (ret != ::android::OK) {
            return;
        }
        mUsage = buf->getUsage();
        AHardwareBuffer_acquire(pBuf);
        mBuf = pBuf;
        mFence = fence;
        mInit = true;

        const uint64_t created =
                sDvBufCreated.fetch_add(1, std::memory_order_relaxed) + 1;
        const uint64_t destroyed =
                sDvBufDestroyed.load(std::memory_order_relaxed);

        ALOGE("DVBUF CREATE source=GraphicBuffer item=%p ahwb=%p id=%llu slot=%d gen=%u usage=0x%llx created=%llu destroyed=%llu alive=%lld",
              static_cast<void*>(this),
              static_cast<void*>(mBuf),
              (unsigned long long)mId,
              mSlot,
              mGeneration,
              (unsigned long long)mUsage,
              (unsigned long long)created,
              (unsigned long long)destroyed,
              (long long)created - (long long)destroyed);
    }
}

LegacyGraphicsTracker::BufferItem::BufferItem(uint32_t generation, AHardwareBuffer* pBuf,
                                              uint64_t usage)
    : mInit{true},
      mGeneration{generation},
      mSlot{-1},
      mBuf{pBuf},
      mUsage{usage},
      mFence{Fence::NO_FENCE} {
    if (__builtin_available(android __ANDROID_API_T__, *)) {
        int ret = AHardwareBuffer_getId(mBuf, &mId);
        if (ret != ::android::OK) {
            mInit = false;
            mBuf = nullptr;
            return;
        }
    }
    AHardwareBuffer_acquire(mBuf);

    const uint64_t created =
            sDvBufCreated.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t destroyed =
            sDvBufDestroyed.load(std::memory_order_relaxed);

    ALOGE("DVBUF CREATE source=AHardwareBuffer item=%p ahwb=%p id=%llu slot=%d gen=%u usage=0x%llx created=%llu destroyed=%llu alive=%lld",
          static_cast<void*>(this),
          static_cast<void*>(mBuf),
          (unsigned long long)mId,
          mSlot,
          mGeneration,
          (unsigned long long)mUsage,
          (unsigned long long)created,
          (unsigned long long)destroyed,
          (long long)created - (long long)destroyed);
}

LegacyGraphicsTracker::BufferItem::~BufferItem() {
    const uint64_t created =
            sDvBufCreated.load(std::memory_order_relaxed);

    uint64_t destroyed =
            sDvBufDestroyed.load(std::memory_order_relaxed);

    if (mInit) {
        AHardwareBuffer_release(mBuf);

        destroyed =
                sDvBufDestroyed.fetch_add(1, std::memory_order_relaxed) + 1;

        ALOGE("DVBUF DESTROY item=%p ahwb=%p id=%llu slot=%d gen=%u usage=0x%llx created=%llu destroyed=%llu alive=%lld",
              static_cast<void*>(this),
              static_cast<void*>(mBuf),
              (unsigned long long)mId,
              mSlot,
              mGeneration,
              (unsigned long long)mUsage,
              (unsigned long long)created,
              (unsigned long long)destroyed,
              (long long)created - (long long)destroyed);
    } else {
        ALOGE("DVBUF DESTROY item=%p init=0 id=%llu slot=%d gen=%u created=%llu destroyed=%llu alive=%lld",
              static_cast<void*>(this),
              (unsigned long long)mId,
              mSlot,
              mGeneration,
              (unsigned long long)created,
              (unsigned long long)destroyed,
              (long long)created - (long long)destroyed);
    }
}

std::shared_ptr<LegacyGraphicsTracker::BufferItem> LegacyGraphicsTracker::BufferItem::migrateBuffer(
        uint64_t newUsage, uint32_t newGeneration) {
    if (!mInit) {
        return nullptr;
    }
    newUsage |= mUsage;
    uint64_t ahbUsage = ::android::AHardwareBuffer_convertFromGrallocUsageBits(newUsage);
    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(mBuf, &desc);
    // TODO: we need well-established buffer migration features from graphics.
    // (b/273776738)
    desc.usage = ahbUsage;
    const native_handle_t* handle = AHardwareBuffer_getNativeHandle(mBuf);
    if (!handle) {
        return nullptr;
    }

    AHardwareBuffer* newBuf;
    int err = AHardwareBuffer_createFromHandle(
            &desc, handle, AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_CLONE, &newBuf);
    if (err != ::android::NO_ERROR) {
        return nullptr;
    }

    std::shared_ptr<BufferItem> newBuffer =
            std::make_shared<BufferItem>(newGeneration, newBuf, newUsage);
    AHardwareBuffer_release(newBuf);
    return newBuffer;
}

sp<GraphicBuffer> LegacyGraphicsTracker::BufferItem::getGraphicBuffer() {
    if (!mInit) {
        return nullptr;
    }
    GraphicBuffer* gb = ::android::AHardwareBuffer_to_GraphicBuffer(mBuf);
    if (!gb) {
        return nullptr;
    }
    gb->setGenerationNumber(mGeneration);
    return gb;
}

LegacyGraphicsTracker::BufferCache::~BufferCache() {
    ALOGV("BufferCache destruction: generation(%d), igbp(%d)", mGeneration, (bool)mIgbp);
}

void LegacyGraphicsTracker::BufferCache::waitOnSlot(int slot) {
    // TODO: log
    CHECK(0 <= slot && slot < kNumSlots);
    BlockedSlot* p = &mBlockedSlots[slot];
    std::unique_lock<std::mutex> l(p->l);
    while (p->blocked) {
        p->cv.wait(l);
    }
}

void LegacyGraphicsTracker::BufferCache::blockSlot(int slot) {
    CHECK(0 <= slot && slot < kNumSlots);
    ALOGV("block slot %d", slot);
    BlockedSlot* p = &mBlockedSlots[slot];
    std::unique_lock<std::mutex> l(p->l);
    p->blocked = true;
}

void LegacyGraphicsTracker::BufferCache::unblockSlot(int slot) {
    CHECK(0 <= slot && slot < kNumSlots);
    ALOGV("unblock slot %d", slot);
    BlockedSlot* p = &mBlockedSlots[slot];
    std::unique_lock<std::mutex> l(p->l);
    p->blocked = false;
    l.unlock();
    p->cv.notify_one();
}

LegacyGraphicsTracker::LegacyGraphicsTracker(int maxDequeueCount)
    : mBufferCache(new BufferCache()),
      mNumDequeueing{0},
      mMaxDequeue{maxDequeueCount},
      mMaxDequeueCommitted{maxDequeueCount},
      mDequeueable{maxDequeueCount},
      mTotalDequeued{0},
      mTotalCancelled{0},
      mTotalDropped{0},
      mTotalReleased{0},
      mInConfig{false},
      mStopped{false},
      mStopRequested{false},
      mAllocAfterStopRequested{0} {
    if (maxDequeueCount < kMaxDequeueMin) {
        mMaxDequeue = kMaxDequeueMin;
        mMaxDequeueCommitted = kMaxDequeueMin;
        mDequeueable = kMaxDequeueMin;
    } else if (maxDequeueCount > kMaxDequeueMax) {
        mMaxDequeue = kMaxDequeueMax;
        mMaxDequeueCommitted = kMaxDequeueMax;
        mDequeueable = kMaxDequeueMax;
    }
    int pipefd[2] = {-1, -1};
    int ret = ::pipe2(pipefd, O_CLOEXEC | O_NONBLOCK);

    mReadPipeFd.reset(pipefd[0]);
    mWritePipeFd.reset(pipefd[1]);

    // ctor does not require lock to be held.
    writeIncDequeueableLocked(mDequeueable);

    CHECK(ret >= 0);
}

LegacyGraphicsTracker::~LegacyGraphicsTracker() {
    stop();
}

bool LegacyGraphicsTracker::adjustDequeueConfLocked(bool* updateDequeue) {
    // TODO: can't we adjust during config? not committing it may safe?
    *updateDequeue = false;
    if (!mInConfig && mMaxDequeueRequested.has_value() && mMaxDequeueRequested < mMaxDequeue) {
        int delta = mMaxDequeue - mMaxDequeueRequested.value();
        int drained = 0;
        // Since we are supposed to increase mDequeuable by one already
        int adjustable = mDequeueable + 1;
        if (adjustable >= delta) {
            mMaxDequeue = mMaxDequeueRequested.value();
            mDequeueable -= (delta - 1);
            drained = delta - 1;
        } else {
            mMaxDequeue -= adjustable;
            drained = mDequeueable;
            mDequeueable = 0;
        }
        if (drained > 0) {
            drainDequeueableLocked(drained);
        }
        if (mMaxDequeueRequested == mMaxDequeue && mMaxDequeueRequested != mMaxDequeueCommitted) {
            *updateDequeue = true;
        }
        return true;
    }
    return false;
}

c2_status_t LegacyGraphicsTracker::configureGraphics(const sp<IGraphicBufferProducer>& igbp,
                                                     uint32_t generation) {
    // TODO: wait until operations to previous IGBP is completed.
    std::shared_ptr<BufferCache> prevCache;
    int prevDequeueRequested = 0;
    int prevDequeueCommitted;

    std::unique_lock<std::mutex> cl(mConfigLock);
    {
        std::unique_lock<std::mutex> l(mLock);
        mInConfig = true;
        prevCache = mBufferCache;
        prevDequeueCommitted = mMaxDequeueCommitted;
        if (mMaxDequeueRequested.has_value()) {
            prevDequeueRequested = mMaxDequeueRequested.value();
        }
    }
    // NOTE: Switching to the same surface is blocked from MediaCodec.
    // Switching to the same surface might not work if tried, since disconnect()
    // to the old surface in MediaCodec and allocate from the new surface from
    // GraphicsTracker cannot be synchronized properly.
    uint64_t bqId{0ULL};
    uint64_t bqUsage{0ULL};
    ::android::status_t ret = ::android::OK;
    if (igbp) {
        ret = igbp->getUniqueId(&bqId);
        if (ret == ::android::OK) {
            (void)igbp->getConsumerUsage(&bqUsage);
        }
    }
    if (ret != ::android::OK || prevCache->mGeneration == generation) {
        ALOGE("new surface configure fail due to wrong or same bqId or same generation:"
              "igbp(%d:%llu -> %llu), gen(%lu -> %lu)",
              (bool)igbp, (unsigned long long)prevCache->mBqId, (unsigned long long)bqId,
              (unsigned long)prevCache->mGeneration, (unsigned long)generation);
        std::unique_lock<std::mutex> l(mLock);
        mInConfig = false;
        return C2_BAD_VALUE;
    }
    ALOGD("new surface in configuration: maxDequeueRequested(%d), maxDequeueCommitted(%d)",
          prevDequeueRequested, prevDequeueCommitted);
    if (prevDequeueRequested > 0 && prevDequeueRequested > prevDequeueCommitted) {
        prevDequeueCommitted = prevDequeueRequested;
    }
    if (igbp) {
        ret = igbp->setMaxDequeuedBufferCount(prevDequeueCommitted);
        if (ret != ::android::OK) {
            ALOGE("new surface maxDequeueBufferCount configure fail");
            // TODO: sort out the error from igbp and return an error accordingly.
            std::unique_lock<std::mutex> l(mLock);
            mInConfig = false;
            return C2_CORRUPTED;
        }
    }
    ALOGD("new surface configured with id:%llu gen:%lu maxDequeue:%d", (unsigned long long)bqId,
          (unsigned long)generation, prevDequeueCommitted);
    std::shared_ptr<BufferCache> newCache =
            std::make_shared<BufferCache>(bqId, bqUsage, generation, igbp);
    {
        std::unique_lock<std::mutex> l(mLock);
        mInConfig = false;
        mBufferCache = newCache;
        // {@code dequeued} is the number of currently dequeued buffers.
        // {@code prevDequeueCommitted} is max dequeued buffer at any moment
        //  from the new surface.
        // {@code newDequeueable} is hence the current # of dequeueable buffers
        //  if no change occurs.
        int dequeued = mDequeued.size() + mNumDequeueing;
        int newDequeueable = prevDequeueCommitted - dequeued;
        if (newDequeueable < 0) {
            // This will not happen.
            // But if this happens, we respect the value and try to continue.
            ALOGE("calculated new dequeueable is negative: %d max(%d),dequeued(%d)", newDequeueable,
                  prevDequeueCommitted, dequeued);
        }

        if (mMaxDequeueRequested.has_value() && mMaxDequeueRequested == prevDequeueCommitted) {
            mMaxDequeueRequested.reset();
        }
        mMaxDequeue = mMaxDequeueCommitted = prevDequeueCommitted;

        int delta = newDequeueable - mDequeueable;
        if (delta > 0) {
            writeIncDequeueableLocked(delta);
        } else if (delta < 0) {
            drainDequeueableLocked(-delta);
        }
        ALOGV("new surfcace dequeueable %d(delta %d), maxDequeue %d", newDequeueable, delta,
              mMaxDequeue);
        mDequeueable = newDequeueable;
    }
    return C2_OK;
}

c2_status_t LegacyGraphicsTracker::configureMaxDequeueCount(int maxDequeueCount) {
    std::shared_ptr<BufferCache> cache;

    if (maxDequeueCount < kMaxDequeueMin || maxDequeueCount > kMaxDequeueMax) {
        ALOGE("max dequeue count %d is not valid", maxDequeueCount);
        return C2_BAD_VALUE;
    }

    // max dequeue count which can be committed to IGBP.
    // (Sometimes maxDequeueCount cannot be committed if the number of
    // dequeued buffer count is bigger.)
    int maxDequeueToCommit;
    std::unique_lock<std::mutex> cl(mConfigLock);
    {
        std::unique_lock<std::mutex> l(mLock);
        if (mMaxDequeueRequested.has_value()) {
            if (mMaxDequeueRequested == maxDequeueCount) {
                ALOGD("maxDequeueCount requested with %d already", maxDequeueCount);
                return C2_OK;
            }
        } else if (mMaxDequeue == maxDequeueCount) {
            ALOGD("maxDequeueCount is already %d", maxDequeueCount);
            return C2_OK;
        }
        mInConfig = true;
        mMaxDequeueRequested = maxDequeueCount;
        cache = mBufferCache;
        if (mMaxDequeue <= maxDequeueCount) {
            maxDequeueToCommit = maxDequeueCount;
        } else {
            // Since mDequeuable is decreasing,
            // a delievered ready to allocate event may not be fulfilled.
            // Another waiting via a waitable object may be necessary in the case.
            int delta = std::min(mMaxDequeue - maxDequeueCount, mDequeueable);
            maxDequeueToCommit = mMaxDequeue - delta;
            mDequeueable -= delta;
            if (delta > 0) {
                drainDequeueableLocked(delta);
            }
        }
    }

    bool committed = true;
    if (cache->mIgbp && maxDequeueToCommit != mMaxDequeueCommitted) {
        ::android::status_t ret = cache->mIgbp->setMaxDequeuedBufferCount(maxDequeueToCommit);
        committed = (ret == ::android::OK);
        if (committed) {
            ALOGD("maxDequeueCount committed to IGBP: %d", maxDequeueToCommit);
        } else {
            // This should not happen.
            ALOGE("maxdequeueCount update to IGBP failed with error(%d)", (int)ret);
        }
    }

    int oldMaxDequeue = 0;
    int requested = 0;
    {
        std::unique_lock<std::mutex> l(mLock);
        mInConfig = false;
        oldMaxDequeue = mMaxDequeue;
        mMaxDequeue = maxDequeueToCommit;  // we already drained dequeueable
        if (committed) {
            clearCacheIfNecessaryLocked(cache, maxDequeueToCommit);
            mMaxDequeueCommitted = maxDequeueToCommit;
            if (mMaxDequeueRequested == mMaxDequeueCommitted &&
                mMaxDequeueRequested == mMaxDequeue) {
                mMaxDequeueRequested.reset();
            }
            if (mMaxDequeueRequested.has_value()) {
                requested = mMaxDequeueRequested.value();
            }
            int delta = mMaxDequeueCommitted - oldMaxDequeue;
            if (delta > 0) {
                mDequeueable += delta;
                writeIncDequeueableLocked(delta);
            }
        }
    }
    ALOGD("maxDqueueCount change %d -> %d: pending: %d", oldMaxDequeue, maxDequeueToCommit,
          requested);

    if (!committed) {
        return C2_CORRUPTED;
    }
    return C2_OK;
}

void LegacyGraphicsTracker::updateDequeueConf() {
    std::shared_ptr<BufferCache> cache;
    int dequeueCommit;
    ALOGV("trying to update max dequeue count");
    std::unique_lock<std::mutex> cl(mConfigLock);
    {
        std::unique_lock<std::mutex> l(mLock);
        if (!mMaxDequeueRequested.has_value() || mMaxDequeue != mMaxDequeueRequested) {
            return;
        }
        if (mMaxDequeueCommitted == mMaxDequeueRequested) {
            // already committed. may not happen.
            mMaxDequeueRequested.reset();
            return;
        }
        dequeueCommit = mMaxDequeue;
        mInConfig = true;
        cache = mBufferCache;
    }
    bool committed = true;
    if (cache->mIgbp) {
        ::android::status_t ret = cache->mIgbp->setMaxDequeuedBufferCount(dequeueCommit);
        committed = (ret == ::android::OK);
        if (committed) {
            ALOGD("delayed maxDequeueCount update to IGBP: %d", dequeueCommit);
        } else {
            // This should not happen.
            ALOGE("delayed maxdequeueCount update to IGBP failed with error(%d)", (int)ret);
        }
    }
    {
        // cache == mCache here, since we locked config.
        std::unique_lock<std::mutex> l(mLock);
        mInConfig = false;
        if (committed) {
            clearCacheIfNecessaryLocked(cache, dequeueCommit);
            mMaxDequeueCommitted = dequeueCommit;
        }
        mMaxDequeueRequested.reset();
    }
}

void LegacyGraphicsTracker::clearCacheIfNecessaryLocked(const std::shared_ptr<BufferCache>& cache,
                                                        int maxDequeueCommitted) {
    int cleared = 0;
    size_t origCacheSize = cache->mBuffers.size();
    if (cache->mIgbp && maxDequeueCommitted < mMaxDequeueCommitted) {
        // we are shrinking # of buffers in the case, so evict the previous
        // cached buffers.
        for (auto it = cache->mBuffers.begin(); it != cache->mBuffers.end();) {
            uint64_t bid = it->second->mId;
            if (mDequeued.count(bid) == 0 || mDeallocating.count(bid) > 0) {
                ++cleared;
                it = cache->mBuffers.erase(it);
            } else {
                ++it;
            }
        }
    }
    ALOGD("Cache size %zu -> %zu: maybe_cleared(%d), dequeued(%zu)", origCacheSize,
          cache->mBuffers.size(), cleared, mDequeued.size());
}

int LegacyGraphicsTracker::getCurDequeueable() {
    std::unique_lock<std::mutex> l(mLock);
    return mDequeueable;
}

void LegacyGraphicsTracker::stop() {
    // TODO: wait until all operation to current IGBP
    // being completed.
    std::unique_lock<std::mutex> l(mLock);
    if (mStopped) {
        return;
    }
    mStopped = true;
    int writeFd = mWritePipeFd.release();
    if (writeFd >= 0) {
        ::close(writeFd);
    }
}

void LegacyGraphicsTracker::onRequestStop() {
    std::unique_lock<std::mutex> l(mLock);
    if (mStopped) {
        return;
    }
    if (mStopRequested) {
        return;
    }
    if (mBufferCache && mBufferCache->mBqId != 0) {
        mReleaseSurface.reset(new PlaceHolderSurface(mBufferCache->mUsage));
    }
    mStopRequested = true;
    writeIncDequeueableLocked(kMaxDequeueMax - 1);
}

void LegacyGraphicsTracker::writeIncDequeueableLocked(int inc) {
    CHECK(inc > 0 && inc < kMaxDequeueMax);
    thread_local char buf[kMaxDequeueMax];
    if (mStopped) {  // reading end closed;
        return;
    }
    int writeFd = mWritePipeFd.get();
    if (writeFd < 0) {
        // initialization fail and not valid though.
        return;
    }
    int ret = ::write(writeFd, buf, inc);
    // Since this is non-blocking i/o, it never returns EINTR.
    //
    // ::write() to pipe guarantee to succeed atomically if it writes less than
    // the given PIPE_BUF. And the buffer size in pipe/fifo is at least 4K and our total
    // max pending buffer size is 64. So it never returns EAGAIN here either.
    // See pipe(7) for further information.
    //
    // Other errors are serious errors and we cannot synchronize mDequeueable to
    // length of pending buffer in pipe/fifo anymore. So better to abort here.
    // TODO: do not abort here. (b/318717399)
    CHECK(ret == inc);
}

void LegacyGraphicsTracker::drainDequeueableLocked(int dec) {
    CHECK(dec > 0 && dec < kMaxDequeueMax);
    thread_local char buf[kMaxDequeueMax];
    if (mStopped) {
        return;
    }
    int readFd = mReadPipeFd.get();
    if (readFd < 0) {
        // initializationf fail and not valid though.
        return;
    }
    int ret = ::read(readFd, buf, dec);
    // TODO: no dot abort here. (b/318717399)
    CHECK(ret == dec);
}

c2_status_t LegacyGraphicsTracker::getWaitableFd(int* pipeFd) {
    *pipeFd = ::dup(mReadPipeFd.get());
    if (*pipeFd < 0) {
        if (mReadPipeFd.get() < 0) {
            return C2_BAD_STATE;
        }
        // dup error
        ALOGE("dup() for the reading end failed %d", errno);
        return C2_NO_MEMORY;
    }
    return C2_OK;
}

c2_status_t LegacyGraphicsTracker::requestAllocateLocked(std::shared_ptr<BufferCache>* cache) {
    if (mDequeueable > 0) {
        char buf[1];
        int ret = ::read(mReadPipeFd.get(), buf, 1);
        if (ret < 0) {
            if (errno == EINTR) {
                // Do we really need to care for cancel due to signal handling?
                return C2_CANCELED;
            }
            if (errno == EAGAIN) {
                // proper usage of waitable object should not return this.
                // but there could be alloc requests from HAL ignoring the internal status.
                return C2_BLOCKING;
            }
            CHECK(errno != 0);
        }
        if (ret == 0) {
            // writing end is closed
            ALOGE("writing end for the waitable object seems to be closed");
            return C2_BAD_STATE;
        }
        mNumDequeueing++;
        mDequeueable--;
        *cache = mBufferCache;
        return C2_OK;
    }
    return C2_BLOCKING;
}

// If {@code cached} is {@code true}, {@code pBuffer} should be read from the
// current cached status. Otherwise, {@code pBuffer} should be written to
// current caches status.
void LegacyGraphicsTracker::commitAllocate(c2_status_t res,
                                           const std::shared_ptr<BufferCache>& cache, bool cached,
                                           int slot, const sp<Fence>& fence,
                                           std::shared_ptr<BufferItem>* pBuffer,
                                           bool* updateDequeue) {
    std::unique_lock<std::mutex> l(mLock);
    mNumDequeueing--;
    if (res == C2_OK) {
        if (cached) {
            auto it = cache->mBuffers.find(slot);
            CHECK(it != cache->mBuffers.end());
            it->second->mFence = fence;
            *pBuffer = it->second;
            ALOGV("an allocated buffer already cached, updated Fence");
        } else if (cache.get() == mBufferCache.get() && mBufferCache->mIgbp) {
            // Cache the buffer if it is allocated from the current IGBP
            CHECK(slot >= 0);
            auto ret = mBufferCache->mBuffers.emplace(slot, *pBuffer);
            if (!ret.second) {
                ret.first->second = *pBuffer;
            }
            ALOGV("an allocated buffer not cached from the current IGBP");
        }
        uint64_t bid = (*pBuffer)->mId;
        auto mapRet = mDequeued.emplace(bid, *pBuffer);
        CHECK(mapRet.second);
    } else {
        ALOGD("allocate error(%d): Dequeued(%zu), Dequeuable(%d)", (int)res, mDequeued.size(),
              mDequeueable + 1);
        if (adjustDequeueConfLocked(updateDequeue)) {
            return;
        }
        mDequeueable++;
        writeIncDequeueableLocked(1);
    }
}

// if a buffer is newly allocated, {@code cached} is {@code false},
// and the buffer is in the {@code buffer}
// otherwise, {@code cached} is {@code false} and the buffer should be
// retrieved by commitAllocate();
c2_status_t LegacyGraphicsTracker::_allocate(const std::shared_ptr<BufferCache>& cache,
                                             uint32_t width, uint32_t height, PixelFormat format,
                                             uint64_t usage, bool* cached, int* rSlotId,
                                             sp<Fence>* rFence,
                                             std::shared_ptr<BufferItem>* buffer) {
    ::android::sp<IGraphicBufferProducer> igbp = cache->mIgbp;
    uint32_t generation = cache->mGeneration;
    if (!igbp) {
        // allocate directly
        AHardwareBuffer_Desc desc;
        desc.width = width;
        desc.height = height;
        desc.layers = 1u;
        desc.format = ::android::AHardwareBuffer_convertFromPixelFormat(format);
        desc.usage = ::android::AHardwareBuffer_convertFromGrallocUsageBits(usage);
        desc.rfu0 = 0;
        desc.rfu1 = 0;

        AHardwareBuffer* buf;
        int ret = AHardwareBuffer_allocate(&desc, &buf);
        if (ret != ::android::OK) {
            ALOGE("direct allocation of AHB failed(%d)", ret);
            return ret == ::android::NO_MEMORY ? C2_NO_MEMORY : C2_CORRUPTED;
        }
        *cached = false;
        *rSlotId = -1;
        *rFence = Fence::NO_FENCE;
        *buffer = std::make_shared<BufferItem>(generation, buf, usage);
        AHardwareBuffer_release(buf);  // remove an acquire count from
                                       // AHwb_allocate().
        if (!*buffer) {
            ALOGE("direct allocation of AHB successful, but failed to create BufferItem");
            return C2_NO_MEMORY;
        }
        if (!(*buffer)->mInit) {
            ALOGE("direct allocation of AHB successful, but BufferItem init failed");
            buffer->reset();
            return C2_CORRUPTED;
        }
        ALOGV("allocate: direct allocate without igbp");
        return C2_OK;
    }

    int slotId;
    uint64_t outBufferAge;
    sp<Fence> fence;

    ::android::status_t status = igbp->dequeueBuffer(&slotId, &fence, width, height, format, usage,
                                                     &outBufferAge, nullptr);
    if (status < ::android::OK) {
        if (status == ::android::TIMED_OUT || status == ::android::WOULD_BLOCK) {
            ALOGW("BQ might not be ready for dequeueBuffer()");
            return C2_BLOCKING;
        }
        bool cacheExpired = false;
        {
            std::unique_lock<std::mutex> l(mLock);
            cacheExpired = (mBufferCache.get() != cache.get());
        }
        if (cacheExpired) {
            ALOGW("a new BQ is configured. dequeueBuffer() error %d", (int)status);
            return C2_BLOCKING;
        }
        ALOGE("BQ in inconsistent status. dequeueBuffer() error %d", (int)status);
        return C2_CORRUPTED;
    }
    cache->waitOnSlot(slotId);
    bool exists = false;
    {
        std::unique_lock<std::mutex> l(mLock);
        if (cache.get() == mBufferCache.get() &&
            cache->mBuffers.find(slotId) != cache->mBuffers.end()) {
            exists = true;
        }
    }
    bool needsRealloc = status & IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION;
    if (needsRealloc || !exists) {
        sp<GraphicBuffer> realloced;
        status = igbp->requestBuffer(slotId, &realloced);
        if (status != ::android::OK) {
            ALOGE("allocate by dequeueBuffer() successful, but requestBuffer() failed %d", status);
            igbp->cancelBuffer(slotId, fence);
            // This might be due to life-cycle end and/or surface switching.
            return C2_BLOCKING;
        }
        *buffer = std::make_shared<BufferItem>(generation, slotId, realloced, fence);
        if (!*buffer) {
            ALOGE("allocate by dequeueBuffer() successful, but creating BufferItem failed");
            igbp->cancelBuffer(slotId, fence);
            return C2_NO_MEMORY;
        }
        if (!(*buffer)->mInit) {
            ALOGE("allocate by dequeueBuffer() successful, but BufferItem init failed");
            buffer->reset();
            igbp->cancelBuffer(slotId, fence);
            return C2_CORRUPTED;
        }
        *cached = false;
    } else {
        *cached = true;
    }
    ALOGV("allocate: a new allocated buffer from igbp cached %d, slot: %d", *cached, slotId);
    *rSlotId = slotId;
    *rFence = fence;
    return C2_OK;
}

c2_status_t LegacyGraphicsTracker::_allocateDirect(uint32_t width, uint32_t height,
                                                   PixelFormat format, uint64_t usage,
                                                   AHardwareBuffer** buf, sp<Fence>* rFence) {
    AHardwareBuffer_Desc desc;
    desc.width = width;
    desc.height = height;
    desc.layers = 1u;
    desc.format = ::android::AHardwareBuffer_convertFromPixelFormat(format);
    desc.usage = ::android::AHardwareBuffer_convertFromGrallocUsageBits(usage);
    desc.rfu0 = 0;
    desc.rfu1 = 0;

    int res = AHardwareBuffer_allocate(&desc, buf);
    if (res != ::android::OK) {
        ALOGE("_allocateDirect() failed(%d)", res);
        if (res == ::android::NO_MEMORY) {
            return C2_NO_MEMORY;
        } else {
            return C2_CORRUPTED;
        }
    }

    *rFence = Fence::NO_FENCE;
    return C2_OK;
}

c2_status_t LegacyGraphicsTracker::allocate(uint32_t width, uint32_t height, PixelFormat format,
                                            uint64_t usage, AHardwareBuffer** buf,
                                            sp<Fence>* rFence) {
    if (mStopped.load() == true) {
        ALOGE("cannot allocate due to being stopped");
        return C2_BAD_STATE;
    }
    c2_status_t res = C2_OK;
    std::shared_ptr<BufferCache> cache;
    {
        std::unique_lock<std::mutex> l(mLock);
        if (mStopRequested) {
            l.unlock();
            if (mReleaseSurface) {
                res = mReleaseSurface->allocate(width, height, format, usage, buf, rFence);
            } else {
                res = _allocateDirect(width, height, format, usage, buf, rFence);
            }
            if (res == C2_OK) {
                ALOGD("allocateed %d buffer after stop", ++mAllocAfterStopRequested);
            }
            // Delay a little bit for HAL to receive stop()/release() request.
            ::usleep(kAllocateDirectDelayUs);
            return res;
        }
        c2_status_t res = requestAllocateLocked(&cache);
        if (res != C2_OK) {
            return res;
        }
    }
    ALOGV("allocatable or dequeueable");

    bool cached = false;
    int slotId;
    sp<Fence> fence;
    std::shared_ptr<BufferItem> buffer;
    bool updateDequeue;
    res = _allocate(cache, width, height, format, usage, &cached, &slotId, &fence, &buffer);
    commitAllocate(res, cache, cached, slotId, fence, &buffer, &updateDequeue);
    if (res == C2_OK) {
        ALOGV("allocated a buffer width:%u height:%u pixelformat:%d usage:%llu", width, height,
              format, (unsigned long long)usage);
        *buf = buffer->mBuf;
        *rFence = buffer->mFence;
        // *buf should be valid even if buffer is dtor-ed.
        AHardwareBuffer_acquire(*buf);
    }
    if (updateDequeue) {
        updateDequeueConf();
    }
    return res;
}

c2_status_t LegacyGraphicsTracker::requestDeallocate(uint64_t bid, const sp<Fence>& fence,
                                                     bool* completed, bool* updateDequeue,
                                                     std::shared_ptr<BufferCache>* cache,
                                                     int* slotId, sp<Fence>* rFence) {
    std::unique_lock<std::mutex> l(mLock);
    if (mDeallocating.find(bid) != mDeallocating.end()) {
        ALOGE("Tries to deallocate a buffer which is already deallocating or rendering");
        return C2_DUPLICATE;
    }
    auto it = mDequeued.find(bid);
    if (it == mDequeued.end()) {
        ALOGE("Tried to deallocate non dequeued buffer");
        return C2_NOT_FOUND;
    }

    std::shared_ptr<BufferItem> buffer = it->second;
    if (buffer->mGeneration == mBufferCache->mGeneration && mBufferCache->mIgbp) {
        auto it = mBufferCache->mBuffers.find(buffer->mSlot);
        CHECK(it != mBufferCache->mBuffers.end() && it->second.get() == buffer.get());
        *cache = mBufferCache;
        *slotId = buffer->mSlot;
        *rFence = (fence == Fence::NO_FENCE) ? buffer->mFence : fence;
        // mark this deallocating
        mDeallocating.emplace(bid);
        mBufferCache->blockSlot(buffer->mSlot);
        *completed = false;
    } else {  // buffer is not from the current underlying Graphics.
        mDequeued.erase(bid);
        *completed = true;
        if (adjustDequeueConfLocked(updateDequeue)) {
            return C2_OK;
        }
        mDequeueable++;
        writeIncDequeueableLocked(1);
    }
    return C2_OK;
}

void LegacyGraphicsTracker::commitDeallocate(std::shared_ptr<BufferCache>& cache, int slotId,
                                             uint64_t bid, bool* updateDequeue) {
    std::unique_lock<std::mutex> l(mLock);
    size_t del1 = mDequeued.erase(bid);
    size_t del2 = mDeallocating.erase(bid);
    CHECK(del1 > 0 && del2 > 0);
    if (cache) {
        cache->unblockSlot(slotId);
    }
    if (adjustDequeueConfLocked(updateDequeue)) {
        return;
    }
    mDequeueable++;
    writeIncDequeueableLocked(1);
}

c2_status_t LegacyGraphicsTracker::deallocate(uint64_t bid, const sp<Fence>& fence) {
    bool completed;
    bool updateDequeue;
    std::shared_ptr<BufferCache> cache;
    int slotId;
    sp<Fence> rFence;
    if (mStopped.load() == true) {
        ALOGE("cannot deallocate due to being stopped");
        return C2_BAD_STATE;
    }
    c2_status_t res =
            requestDeallocate(bid, fence, &completed, &updateDequeue, &cache, &slotId, &rFence);
    if (res != C2_OK) {
        return res;
    }
    if (completed == true) {
        if (updateDequeue) {
            updateDequeueConf();
        }
        return C2_OK;
    }

    // ignore return value since IGBP could be already stale.
    // cache->mIgbp is not null, if completed is false.
    (void)cache->mIgbp->cancelBuffer(slotId, rFence);

    commitDeallocate(cache, slotId, bid, &updateDequeue);
    if (updateDequeue) {
        updateDequeueConf();
    }
    return C2_OK;
}

c2_status_t LegacyGraphicsTracker::requestAttachForRender(const C2ConstGraphicBlock& blk,
                                                          const sp<Fence>& fence,
                                                          std::shared_ptr<BufferCache>* pCache,
                                                          std::shared_ptr<BufferItem>* pBuffer,
                                                          bool* updateDequeue) {
    const uint64_t attachSeq =
            sDvAttachRequests.fetch_add(1, std::memory_order_relaxed) + 1;

    if (mStopped.load() == true) {
        ALOGE("DVATTACH REQUEST #%llu stopped=1",
              (unsigned long long)attachSeq);
        ALOGE("cannot requestAttachForRender due to being stopped");
        return C2_BAD_STATE;
    }

    // allocate for attach
    c2_status_t res = C2_OK;
    std::shared_ptr<BufferCache> cache;
    size_t cacheBefore = 0;
    size_t dequeuedBefore = 0;
    size_t deallocatingBefore = 0;
    int dequeueableBefore = 0;

    {
        std::unique_lock<std::mutex> l(mLock);
        if (mStopRequested) {
            ALOGE("DVATTACH REQUEST #%llu stopRequested=1",
                  (unsigned long long)attachSeq);
            return C2_BAD_STATE;
        }
        if (!mBufferCache || !(mBufferCache->mIgbp)) {
            ALOGE("DVATTACH REQUEST #%llu no current IGBP",
                  (unsigned long long)attachSeq);
            return C2_OMITTED;
        }

        cacheBefore = mBufferCache->mBuffers.size();
        dequeuedBefore = mDequeued.size();
        deallocatingBefore = mDeallocating.size();
        dequeueableBefore = mDequeueable;

        res = requestAllocateLocked(&cache);
        if (res != C2_OK) {
            ALOGE("DVATTACH REQUEST #%llu reserve failed res=%d cache=%zu dequeued=%zu deallocating=%zu dequeueable=%d",
                  (unsigned long long)attachSeq, res, cacheBefore, dequeuedBefore,
                  deallocatingBefore, dequeueableBefore);
            ALOGV("cannot allocate for requestAttachForRender: %d", res);
            return res;
        }
    }

    // attach to the surface
    ::android::sp<IGraphicBufferProducer> igbp = cache->mIgbp;
    uint32_t generation = cache->mGeneration;
    uint64_t usage = 0ULL;
    int slotId = -1;
    (void)igbp->getConsumerUsage(&usage);

    sp<GraphicBuffer> grBuf = createGraphicBuffer(blk, generation, usage);
    std::shared_ptr<BufferItem> buffer;

    uint64_t ahbId = 0;
    uint32_t ahbWidth = 0;
    uint32_t ahbHeight = 0;
    uint32_t ahbLayers = 0;
    uint32_t ahbFormat = 0;
    uint32_t ahbStride = 0;
    uint64_t ahbUsage = 0;

    if (grBuf) {
        if (__builtin_available(android __ANDROID_API_T__, *)) {
            AHardwareBuffer* ahb = AHardwareBuffer_from_GraphicBuffer(grBuf.get());
            if (ahb) {
                (void)AHardwareBuffer_getId(ahb, &ahbId);
                AHardwareBuffer_Desc desc{};
                AHardwareBuffer_describe(ahb, &desc);
                ahbWidth = desc.width;
                ahbHeight = desc.height;
                ahbLayers = desc.layers;
                ahbFormat = desc.format;
                ahbStride = desc.stride;
                ahbUsage = desc.usage;
            }
        }

        ALOGE("DVATTACH REQUEST #%llu PRE_ATTACH handle=%p gb=%p ahbId=%llu %ux%u layers=%u fmt=%u stride=%u usage=0x%llx cacheBefore=%zu dequeuedBefore=%zu deallocatingBefore=%zu dequeueableBefore=%d",
              (unsigned long long)attachSeq,
              blk.handle(),
              grBuf.get(),
              (unsigned long long)ahbId,
              ahbWidth,
              ahbHeight,
              ahbLayers,
              ahbFormat,
              ahbStride,
              (unsigned long long)ahbUsage,
              cacheBefore,
              dequeuedBefore,
              deallocatingBefore,
              dequeueableBefore);

        ::android::status_t status = igbp->attachBuffer(&slotId, grBuf);

        ALOGE("DVATTACH REQUEST #%llu ATTACH status=%d slot=%d ahbId=%llu",
              (unsigned long long)attachSeq,
              (int)status,
              slotId,
              (unsigned long long)ahbId);

        if (status != ::android::OK) {
            res = C2_REFUSED;
        } else {
            buffer = std::make_shared<BufferItem>(generation, slotId, grBuf, fence);
            if (buffer->mInit) {
                cache->waitOnSlot(slotId);
                *pCache = cache;
                *pBuffer = buffer;
                res = C2_OK;
            } else {
                (void)igbp->cancelBuffer(slotId, Fence::NO_FENCE);
                buffer.reset();
                res = C2_BAD_VALUE;
            }
        }
    } else {
        ALOGE("DVATTACH REQUEST #%llu createGraphicBuffer failed handle=%p",
              (unsigned long long)attachSeq, blk.handle());
        res = C2_BAD_VALUE;
    }

    // Do commitAllocate() regardless of the return value, since commitAllocate()
    // also handles rollback from requestAllocateLocked().
    commitAllocate(res, cache, false, slotId, fence, &buffer, updateDequeue);

    size_t cacheAfterCommit = 0;
    size_t dequeuedAfterCommit = 0;
    size_t deallocatingAfterCommit = 0;
    int dequeueableAfterCommit = 0;

    {
        std::unique_lock<std::mutex> l(mLock);
        cacheAfterCommit = cache ? cache->mBuffers.size() : 0;
        dequeuedAfterCommit = mDequeued.size();
        deallocatingAfterCommit = mDeallocating.size();
        dequeueableAfterCommit = mDequeueable;
    }

    const uint64_t commitSeq =
            sDvAttachCommits.fetch_add(1, std::memory_order_relaxed) + 1;

    ALOGE("DVATTACH COMMIT_ALLOC #%llu request=%llu res=%d slot=%d bufferId=%llu cache=%zu->%zu dequeued=%zu->%zu deallocating=%zu->%zu dequeueable=%d->%d",
          (unsigned long long)commitSeq,
          (unsigned long long)attachSeq,
          res,
          slotId,
          (unsigned long long)(buffer ? buffer->mId : 0),
          cacheBefore,
          cacheAfterCommit,
          dequeuedBefore,
          dequeuedAfterCommit,
          deallocatingBefore,
          deallocatingAfterCommit,
          dequeueableBefore,
          dequeueableAfterCommit);

    if (res == C2_OK) {
        // since attach/allocate was successful, prepare for render here.
        {
            std::unique_lock<std::mutex> l(mLock);
            mDeallocating.emplace(buffer->mId);

            ALOGE("DVATTACH MARK_RENDER request=%llu slot=%d id=%llu cache=%zu dequeued=%zu deallocating=%zu dequeueable=%d",
                  (unsigned long long)attachSeq,
                  buffer->mSlot,
                  (unsigned long long)buffer->mId,
                  cache->mBuffers.size(),
                  mDequeued.size(),
                  mDeallocating.size(),
                  mDequeueable);
        }
        cache->blockSlot(buffer->mSlot);
    }

    return res;
}

c2_status_t LegacyGraphicsTracker::requestRender(uint64_t bid, std::shared_ptr<BufferCache>* cache,
                                                 std::shared_ptr<BufferItem>* pBuffer,
                                                 bool* fromCache, bool* updateDequeue) {
    std::unique_lock<std::mutex> l(mLock);
    if (mDeallocating.find(bid) != mDeallocating.end()) {
        ALOGE("Tries to render a buffer which is already deallocating or rendering");
        return C2_DUPLICATE;
    }
    auto it = mDequeued.find(bid);
    if (it == mDequeued.end()) {
        ALOGE("Tried to render non dequeued buffer");
        return C2_NOT_FOUND;
    }
    if (!mBufferCache->mIgbp) {
        // Render requested without surface.
        // reclaim the buffer for dequeue.
        // TODO: is this correct for API wise?
        mDequeued.erase(it);
        if (adjustDequeueConfLocked(updateDequeue)) {
            return C2_BAD_STATE;
        }
        mDequeueable++;
        writeIncDequeueableLocked(1);
        return C2_BAD_STATE;
    }
    std::shared_ptr<BufferItem> buffer = it->second;
    *cache = mBufferCache;
    if (buffer->mGeneration == mBufferCache->mGeneration) {
        auto it = mBufferCache->mBuffers.find(buffer->mSlot);
        CHECK(it != mBufferCache->mBuffers.end() && it->second.get() == buffer.get());
        mBufferCache->blockSlot(buffer->mSlot);
        *fromCache = true;
    } else {
        *fromCache = false;
    }
    *pBuffer = buffer;
    mDeallocating.emplace(bid);
    return C2_OK;
}

void LegacyGraphicsTracker::commitRender(const std::shared_ptr<BufferCache>& cache,
                                         const std::shared_ptr<BufferItem>& buffer,
                                         const std::shared_ptr<BufferItem>& oldBuffer,
                                         bool bufferReplaced, bool* updateDequeue) {
    std::unique_lock<std::mutex> l(mLock);
    uint64_t origBid = oldBuffer ? oldBuffer->mId : buffer->mId;

    const size_t cacheBefore = cache ? cache->mBuffers.size() : 0;
    const size_t dequeuedBefore = mDequeued.size();
    const size_t deallocatingBefore = mDeallocating.size();
    const int dequeueableBefore = mDequeueable;

    if (cache) {
        cache->unblockSlot(buffer->mSlot);
        if (oldBuffer) {
            // migrated, register the new buffer to the cache.
            auto ret = cache->mBuffers.emplace(buffer->mSlot, buffer);
            if (!ret.second) {
                ret.first->second = buffer;
            }
        }
    }

    const size_t delDeallocating = mDeallocating.erase(origBid);
    const size_t delDequeued = mDequeued.erase(origBid);

    const bool staleCache = cache.get() != mBufferCache.get();

    ALOGE("DVATTACH COMMIT_RENDER slot=%d id=%llu oldId=%llu oldBuffer=%d replaced=%d staleCache=%d cache=%zu->%zu dequeued=%zu->%zu deallocating=%zu->%zu erasedDequeued=%zu erasedDeallocating=%zu dequeueable=%d",
          buffer ? buffer->mSlot : -1,
          (unsigned long long)(buffer ? buffer->mId : 0),
          (unsigned long long)origBid,
          oldBuffer ? 1 : 0,
          bufferReplaced ? 1 : 0,
          staleCache ? 1 : 0,
          cacheBefore,
          cache ? cache->mBuffers.size() : 0,
          dequeuedBefore,
          mDequeued.size(),
          deallocatingBefore,
          mDeallocating.size(),
          delDequeued,
          delDeallocating,
          mDequeueable);

    if (staleCache || bufferReplaced) {
        // Surface changed, no need to wait for buffer being released.
        if (adjustDequeueConfLocked(updateDequeue)) {
            return;
        }
        mDequeueable++;
        writeIncDequeueableLocked(1);

        ALOGE("DVATTACH COMMIT_RENDER RELEASE_IMMEDIATE slot=%d id=%llu dequeueable=%d->%d",
              buffer ? buffer->mSlot : -1,
              (unsigned long long)(buffer ? buffer->mId : 0),
              dequeueableBefore,
              mDequeueable);
        return;
    }

    ALOGE("DVATTACH COMMIT_RENDER WAIT_RELEASE slot=%d id=%llu cache=%zu dequeueable=%d attached=%d",
          buffer ? buffer->mSlot : -1,
          (unsigned long long)(buffer ? buffer->mId : 0),
          cache ? cache->mBuffers.size() : 0,
          mDequeueable,
          cache ? cache->mNumAttached.load(std::memory_order_relaxed) : -1);
}

c2_status_t LegacyGraphicsTracker::render(const C2ConstGraphicBlock& blk,
                                          const IGraphicBufferProducer::QueueBufferInput& input,
                                          IGraphicBufferProducer::QueueBufferOutput* output) {
    const uint64_t renderSeq =
            sDvTrackRender.fetch_add(1, std::memory_order_relaxed) + 1;

    std::shared_ptr<BufferCache> cache;
    std::shared_ptr<BufferItem> buffer;
    uint64_t bid = 0;
    bool updateDequeue = false;
    bool fromCache = false;
    std::shared_ptr<_C2BlockPoolData> poolData = _C2BlockFactory::GetGraphicBlockPoolData(blk);

    if (!poolData) {
        const uint64_t noPool =
                sDvTrackNoPool.fetch_add(1, std::memory_order_relaxed) + 1;

        ALOGE("DVTRACK RENDER #%llu NO_POOL_DATA noPool=%llu igbaPool=%llu handle=%p",
              (unsigned long long)renderSeq,
              (unsigned long long)noPool,
              (unsigned long long)sDvTrackIgbaPool.load(std::memory_order_relaxed),
              blk.handle());

        if (!blk.handle()) {
            ALOGE("DVTRACK RENDER #%llu NO_POOL_DATA has no native_handle",
                  (unsigned long long)renderSeq);
            ALOGE("block does not have native_handle for render");
            return C2_CORRUPTED;
        }

        // This might be a block directly created by gralloc allocator.
        // So we need to attach the block to the surface before render.
        fromCache = true;
        c2_status_t res = requestAttachForRender(blk, input.fence, &cache, &buffer, &updateDequeue);

        ALOGE("DVTRACK RENDER #%llu ATTACH_EXTERNAL res=%d buffer=%p slot=%d",
              (unsigned long long)renderSeq,
              res,
              buffer.get(),
              buffer ? buffer->mSlot : -1);

        if (res != C2_OK) {
            if (updateDequeue) {
                updateDequeueConf();
            }
            ALOGE("attaching external buffer for render failed: %d", res);
            return res;
        }
    } else {
        const uint64_t igbaPool =
                sDvTrackIgbaPool.fetch_add(1, std::memory_order_relaxed) + 1;

        c2_status_t res = retrieveAHardwareBufferId(blk, &bid);

        ALOGE("DVTRACK RENDER #%llu IGBA_POOL_DATA poolData=%p bid=%llu res=%d noPool=%llu igbaPool=%llu",
              (unsigned long long)renderSeq,
              poolData.get(),
              (unsigned long long)bid,
              res,
              (unsigned long long)sDvTrackNoPool.load(std::memory_order_relaxed),
              (unsigned long long)igbaPool);

        if (res != C2_OK) {
            ALOGE("retrieving AHB-ID for GraphicBlock failed");
            return C2_CORRUPTED;
        }

        _C2BlockFactory::DisownIgbaBlock(poolData);

        res = requestRender(bid, &cache, &buffer, &fromCache, &updateDequeue);

        ALOGE("DVTRACK RENDER #%llu REQUEST_RENDER bid=%llu res=%d fromCache=%d buffer=%p slot=%d",
              (unsigned long long)renderSeq,
              (unsigned long long)bid,
              res,
              fromCache ? 1 : 0,
              buffer.get(),
              buffer ? buffer->mSlot : -1);

        if (res != C2_OK) {
            if (updateDequeue) {
                updateDequeueConf();
            }
            return res;
        }
    }

    int cacheSlotId = fromCache ? buffer->mSlot : -1;
    std::shared_ptr<BufferItem> oldBuffer;

    ALOGE("DVTRACK RENDER #%llu PREPARED fromCache=%d slot=%d bid=%llu",
          (unsigned long long)renderSeq,
          fromCache ? 1 : 0,
          cacheSlotId,
          (unsigned long long)bid);

    ALOGV("render prepared: igbp(%d) slot(%d)", bool(cache->mIgbp), cacheSlotId);

    if (!fromCache) {
        const uint64_t migration =
                sDvTrackMigration.fetch_add(1, std::memory_order_relaxed) + 1;

        // The buffer does not come from the current cache.
        // The buffer is needed to be migrated(attached).
        uint64_t newUsage = 0ULL;

        (void)cache->mIgbp->getConsumerUsage(&newUsage);
        std::shared_ptr<BufferItem> newBuffer = buffer->migrateBuffer(newUsage, cache->mGeneration);
        sp<GraphicBuffer> gb = newBuffer ? newBuffer->getGraphicBuffer() : nullptr;

        ALOGE("DVTRACK RENDER #%llu MIGRATE #%llu bid=%llu old=%p new=%p gb=%p usage=0x%llx",
              (unsigned long long)renderSeq,
              (unsigned long long)migration,
              (unsigned long long)bid,
              buffer.get(),
              newBuffer.get(),
              gb.get(),
              (unsigned long long)newUsage);

        if (!gb) {
            ALOGE("render: realloc-ing a new buffer for migration failed");
            std::shared_ptr<BufferCache> nullCache;
            commitDeallocate(nullCache, -1, bid, &updateDequeue);
            if (updateDequeue) {
                updateDequeueConf();
            }
            return C2_REFUSED;
        }

        const ::android::status_t attachRes =
                cache->mIgbp->attachBuffer(&(newBuffer->mSlot), gb);

        ALOGE("DVTRACK RENDER #%llu MIGRATE_ATTACH bid=%llu res=%d slot=%d",
              (unsigned long long)renderSeq,
              (unsigned long long)bid,
              (int)attachRes,
              newBuffer->mSlot);

        if (attachRes != ::android::OK) {
            ALOGE("render: attaching a new buffer to IGBP failed");
            std::shared_ptr<BufferCache> nullCache;
            commitDeallocate(nullCache, -1, bid, &updateDequeue);
            if (updateDequeue) {
                updateDequeueConf();
            }
            return C2_REFUSED;
        }

        cache->waitOnSlot(newBuffer->mSlot);
        cache->blockSlot(newBuffer->mSlot);
        oldBuffer = buffer;
        buffer = newBuffer;
    }

    ::android::status_t renderRes = cache->mIgbp->queueBuffer(buffer->mSlot, input, output);

    const uint64_t queued =
            sDvTrackQueued.fetch_add(1, std::memory_order_relaxed) + 1;

    ALOGE("DVTRACK RENDER #%llu QUEUE #%llu bid=%llu slot=%d migration=%d res=%d replaced=%d",
          (unsigned long long)renderSeq,
          (unsigned long long)queued,
          (unsigned long long)bid,
          buffer->mSlot,
          !fromCache ? 1 : 0,
          (int)renderRes,
          output ? (output->bufferReplaced ? 1 : 0) : -1);

    ALOGV("render done: migration(%d), render(err = %d)", !fromCache, renderRes);

    if (renderRes != ::android::OK) {
        CHECK(renderRes != ::android::BAD_VALUE);
        ALOGE("render: failed to queueBuffer() err = %d", renderRes);
        (void)cache->mIgbp->cancelBuffer(buffer->mSlot, input.fence);
        commitDeallocate(cache, buffer->mSlot, bid, &updateDequeue);
        if (updateDequeue) {
            updateDequeueConf();
        }
        return C2_REFUSED;
    }

    commitRender(cache, buffer, oldBuffer, output->bufferReplaced, &updateDequeue);

    ALOGE("DVTRACK RENDER #%llu COMMIT bid=%llu noPool=%llu igbaPool=%llu migrations=%llu queued=%llu",
          (unsigned long long)renderSeq,
          (unsigned long long)bid,
          (unsigned long long)sDvTrackNoPool.load(std::memory_order_relaxed),
          (unsigned long long)sDvTrackIgbaPool.load(std::memory_order_relaxed),
          (unsigned long long)sDvTrackMigration.load(std::memory_order_relaxed),
          (unsigned long long)sDvTrackQueued.load(std::memory_order_relaxed));

    if (updateDequeue) {
        updateDequeueConf();
    }
    return C2_OK;
}

void LegacyGraphicsTracker::pollForRenderedFrames(FrameEventHistoryDelta* delta) {
    sp<IGraphicBufferProducer> igbp;
    {
        std::unique_lock<std::mutex> l(mLock);
        if (mBufferCache) {
            igbp = mBufferCache->mIgbp;
        }
    }
    if (igbp) {
        igbp->getFrameTimestamps(delta);
    }
}

void LegacyGraphicsTracker::onReleased(uint32_t generation) {
    const uint64_t releaseSeq =
            sDvAttachReleased.fetch_add(1, std::memory_order_relaxed) + 1;

    bool updateDequeue = false;
    {
        std::unique_lock<std::mutex> l(mLock);

        const uint32_t currentGeneration = mBufferCache ? mBufferCache->mGeneration : 0;
        const int attachedBefore = mBufferCache ? mBufferCache->mNumAttached.load(std::memory_order_relaxed) : -1;
        const size_t cacheSize = mBufferCache ? mBufferCache->mBuffers.size() : 0;
        const int dequeueableBefore = mDequeueable;

        if (mBufferCache && mBufferCache->mGeneration == generation) {
            if (mBufferCache->mNumAttached > 0) {
                mBufferCache->mNumAttached--;

                ALOGE("DVATTACH ON_RELEASED #%llu gen=%u currentGen=%u IGNORED_FOR_ATTACHED attached=%d->%d cache=%zu dequeueable=%d",
                      (unsigned long long)releaseSeq,
                      generation,
                      currentGeneration,
                      attachedBefore,
                      mBufferCache->mNumAttached.load(std::memory_order_relaxed),
                      cacheSize,
                      mDequeueable);

                ALOGV("one onReleased() ignored for each prior onAttached().");
                return;
            }

            if (!adjustDequeueConfLocked(&updateDequeue)) {
                mDequeueable++;
                writeIncDequeueableLocked(1);
            }

            ALOGE("DVATTACH ON_RELEASED #%llu gen=%u currentGen=%u RELEASE_TOKEN attached=%d cache=%zu dequeueable=%d->%d update=%d",
                  (unsigned long long)releaseSeq,
                  generation,
                  currentGeneration,
                  attachedBefore,
                  cacheSize,
                  dequeueableBefore,
                  mDequeueable,
                  updateDequeue ? 1 : 0);
        } else {
            ALOGE("DVATTACH ON_RELEASED #%llu gen=%u currentGen=%u STALE cache=%zu attached=%d dequeueable=%d",
                  (unsigned long long)releaseSeq,
                  generation,
                  currentGeneration,
                  cacheSize,
                  attachedBefore,
                  mDequeueable);
        }
    }

    if (updateDequeue) {
        updateDequeueConf();
    }
}

void LegacyGraphicsTracker::onAttached(uint32_t generation) {
    const uint64_t attachCbSeq =
            sDvAttachCallbacks.fetch_add(1, std::memory_order_relaxed) + 1;

    std::unique_lock<std::mutex> l(mLock);

    const uint32_t currentGeneration = mBufferCache ? mBufferCache->mGeneration : 0;
    const int attachedBefore = mBufferCache ? mBufferCache->mNumAttached.load(std::memory_order_relaxed) : -1;
    const size_t cacheSize = mBufferCache ? mBufferCache->mBuffers.size() : 0;

    if (mBufferCache && mBufferCache->mGeneration == generation) {
        mBufferCache->mNumAttached++;

        ALOGE("DVATTACH ON_ATTACHED #%llu gen=%u currentGen=%u attached=%d->%d cache=%zu dequeueable=%d",
              (unsigned long long)attachCbSeq,
              generation,
              currentGeneration,
              attachedBefore,
              mBufferCache->mNumAttached.load(std::memory_order_relaxed),
              cacheSize,
              mDequeueable);

        ALOGV("buffer attached");
    } else {
        ALOGE("DVATTACH ON_ATTACHED #%llu gen=%u currentGen=%u STALE cache=%zu attached=%d dequeueable=%d",
              (unsigned long long)attachCbSeq,
              generation,
              currentGeneration,
              cacheSize,
              attachedBefore,
              mDequeueable);
    }
}

}  // namespace aidl::android::hardware::media::c2::implementation
