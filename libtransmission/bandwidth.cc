/*
 * This file Copyright (C) 2008-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <algorithm>
#include <cstring> /* memset() */

#include "transmission.h"
#include "bandwidth.h"
#include "crypto-utils.h" /* tr_rand_int_weak() */
#include "log.h"
#include "peer-io.h"
#include "tr-assert.h"
#include "utils.h"

#define dbgmsg(...) tr_logAddDeepNamed(nullptr, __VA_ARGS__)

/***
****
***/

unsigned int Bandwidth::getSpeed_Bps(RateControl const* r, unsigned int interval_msec, uint64_t now)
{
    if (now == 0)
    {
        now = tr_time_msec();
    }

    if (now != r->cache_time_)
    {
        uint64_t bytes = 0;
        uint64_t const cutoff = now - interval_msec;
        auto* rvolatile = (RateControl*)r;

        for (int i = r->newest_; r->transfers_[i].date_ > cutoff;)
        {
            bytes += r->transfers_[i].size_;

            if (--i == -1)
            {
                i = HISTORY_SIZE - 1; /* circular history */
            }

            if (i == r->newest_)
            {
                break; /* we've come all the way around */
            }
        }

        rvolatile->cache_val_ = (unsigned int)(bytes * 1000U / interval_msec);
        rvolatile->cache_time_ = now;
    }

    return r->cache_val_;
}

void Bandwidth::notifyBandwidthConsumedBytes(uint64_t const now, RateControl* r, size_t size)
{
    if (r->transfers_[r->newest_].date_ + GRANULARITY_MSEC >= now)
    {
        r->transfers_[r->newest_].size_ += size;
    }
    else
    {
        if (++r->newest_ == HISTORY_SIZE)
        {
            r->newest_ = 0;
        }

        r->transfers_[r->newest_].date_ = now;
        r->transfers_[r->newest_].size_ = size;
    }

    /* invalidate cache_val*/
    r->cache_time_ = 0;
}

/***
****
***/

Bandwidth::Bandwidth(Bandwidth* newParent)
    : band_{}
    , parent_{ nullptr }
    , children_{}
    , peer_{ nullptr }
{
    this->children_ = {};
    this->band_[TR_UP].honor_parent_limits_ = true;
    this->band_[TR_DOWN].honor_parent_limits_ = true;
    this->setParent(newParent);
}

/***
****
***/

void Bandwidth::setParent(Bandwidth* newParent)
{
    TR_ASSERT(this != newParent);

    if (this->parent_ != nullptr)
    {
        this->parent_->children_.erase(this);
        this->parent_ = nullptr;
    }

    if (newParent != nullptr)
    {
        TR_ASSERT(newParent->parent_ != this);
        TR_ASSERT(newParent->children_.find(this) == newParent->children_.end()); // does not exist

        newParent->children_.insert(this);
        this->parent_ = newParent;
    }
}

/***
****
***/

void Bandwidth::allocateBandwidth(
    tr_priority_t parent_priority,
    tr_direction dir,
    unsigned int period_msec,
    std::vector<tr_peerIo*>& peer_pool)
{
    TR_ASSERT(tr_isDirection(dir));

    tr_priority_t const priority_ = std::max(parent_priority, this->priority);

    /* set the available bandwidth */
    if (this->band_[dir].is_limited_)
    {
        uint64_t const nextPulseSpeed = this->band_[dir].desired_speed_bps_;
        this->band_[dir].bytes_left_ = nextPulseSpeed * period_msec / 1000U;
    }

    /* add this bandwidth's peer, if any, to the peer pool */
    if (this->peer_ != nullptr)
    {
        this->peer_->priority = priority_;
        peer_pool.push_back(this->peer_);
    }

    // traverse & repeat for the subtree
    for (auto child : this->children_)
    {
        child->allocateBandwidth(priority_, dir, period_msec, peer_pool);
    }
}

void Bandwidth::phaseOne(std::vector<tr_peerIo*>& peerArray, tr_direction dir)
{
    /* First phase of IO. Tries to distribute bandwidth fairly to keep faster
     * peers from starving the others. Loop through the peers, giving each a
     * small chunk of bandwidth. Keep looping until we run out of bandwidth
     * and/or peers that can use it */
    dbgmsg("%lu peers to go round-robin for %s", peerArray.size(), dir == TR_UP ? "upload" : "download");

    size_t n = peerArray.size();
    while (n > 0)
    {
        int const i = tr_rand_int_weak(n); /* pick a peer at random */

        /* value of 3000 bytes chosen so that when using uTP we'll send a full-size
         * frame right away and leave enough buffered data for the next frame to go
         * out in a timely manner. */
        size_t const increment = 3000;

        int const bytesUsed = tr_peerIoFlush(peerArray[i], dir, increment);

        dbgmsg("peer #%d of %zu used %d bytes in this pass", i, n, bytesUsed);

        if (bytesUsed != (int)increment)
        {
            /* peer is done writing for now; move it to the end of the list */
            std::swap(peerArray[i], peerArray[n - 1]);
            --n;
        }
    }
}

void Bandwidth::allocate(tr_direction dir, unsigned int period_msec)
{
    std::vector<tr_peerIo*> tmp;
    std::vector<tr_peerIo*> low;
    std::vector<tr_peerIo*> normal;
    std::vector<tr_peerIo*> high;

    /* allocateBandwidth () is a helper function with two purposes:
     * 1. allocate bandwidth to b and its subtree
     * 2. accumulate an array of all the peerIos from b and its subtree. */
    this->allocateBandwidth(TR_PRI_LOW, dir, period_msec, tmp);

    for (auto io : tmp)
    {
        tr_peerIoRef(io);
        tr_peerIoFlushOutgoingProtocolMsgs(io);

        switch (io->priority)
        {
        case TR_PRI_HIGH:
            high.push_back(io);
            [[fallthrough]];

        case TR_PRI_NORMAL:
            normal.push_back(io);
            [[fallthrough]];

        default:
            low.push_back(io);
        }
    }

    /* First phase of IO. Tries to distribute bandwidth fairly to keep faster
     * peers from starving the others. Loop through the peers, giving each a
     * small chunk of bandwidth. Keep looping until we run out of bandwidth
     * and/or peers that can use it */
    phaseOne(high, dir);
    phaseOne(normal, dir);
    phaseOne(low, dir);

    /* Second phase of IO. To help us scale in high bandwidth situations,
     * enable on-demand IO for peers with bandwidth left to burn.
     * This on-demand IO is enabled until (1) the peer runs out of bandwidth,
     * or (2) the next Bandwidth::allocate () call, when we start over again. */
    for (auto io : tmp)
    {
        tr_peerIoSetEnabled(io, dir, tr_peerIoHasBandwidthLeft(io, dir));
    }

    for (auto io : tmp)
    {
        tr_peerIoUnref(io);
    }
}

/***
****
***/

unsigned int Bandwidth::clamp(uint64_t now, tr_direction dir, unsigned int byteCount) const
{
    TR_ASSERT(tr_isDirection(dir));

    if (this->band_[dir].is_limited_)
    {
        byteCount = std::min(byteCount, this->band_[dir].bytes_left_);

        /* if we're getting close to exceeding the speed limit,
         * clamp down harder on the bytes available */
        if (byteCount > 0)
        {
            double current;
            double desired;
            double r;

            if (now == 0)
            {
                now = tr_time_msec();
            }

            current = this->getRawSpeed_Bps(now, TR_DOWN);
            desired = this->getDesiredSpeed_Bps(TR_DOWN);
            r = desired >= 1 ? current / desired : 0;

            if (r > 1.0)
            {
                byteCount = 0;
            }
            else if (r > 0.9)
            {
                byteCount = static_cast<unsigned int>(byteCount * 0.8);
            }
            else if (r > 0.8)
            {
                byteCount = static_cast<unsigned int>(byteCount * 0.9);
            }
        }
    }

    if (this->parent_ != nullptr && this->band_[dir].honor_parent_limits_ && byteCount > 0)
    {
        byteCount = this->parent_->clamp(now, dir, byteCount);
    }

    return byteCount;
}

void Bandwidth::notifyBandwidthConsumed(tr_direction dir, size_t byteCount, bool isPieceData, uint64_t now)
{
    TR_ASSERT(tr_isDirection(dir));

    Band* band = &this->band_[dir];

    if (band->is_limited_ && isPieceData)
    {
        band->bytes_left_ -= std::min(size_t{ band->bytes_left_ }, byteCount);
    }

#ifdef DEBUG_DIRECTION

    if (dir == DEBUG_DIRECTION && band_->isLimited)
    {
        fprintf(
            stderr,
            "%p consumed %5zu bytes of %5s data... was %6zu, now %6zu left\n",
            this,
            byteCount,
            isPieceData ? "piece" : "raw",
            oldBytesLeft,
            band_->bytesLeft);
    }

#endif

    notifyBandwidthConsumedBytes(now, &band->raw_, byteCount);

    if (isPieceData)
    {
        notifyBandwidthConsumedBytes(now, &band->piece_, byteCount);
    }

    if (this->parent_ != nullptr)
    {
        this->parent_->notifyBandwidthConsumed(dir, byteCount, isPieceData, now);
    }
}
