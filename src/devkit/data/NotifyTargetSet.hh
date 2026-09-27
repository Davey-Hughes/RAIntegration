#ifndef RA_DATA_NOTIFYTARGET_SET_H
#define RA_DATA_NOTIFYTARGET_SET_H
#pragma once

#include "util/GSL.hh"

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace ra {
namespace data {

/// <summary>
/// A collection of pointers to other objects.
/// </summary>
/// <remarks>
/// These are not allocated objects and do not need to be free'd.
/// This behaves like a set of references, which isn't allowed.
///
/// Safe to use from several threads at once. The targets are kept in an immutable list: Add, Remove and Clear
/// publish a new one, and a pass over the targets - <see cref="Targets" /> or <see cref="ForEachTarget" /> - walks
/// the list that was current when it began. So a change made during a pass, on any thread, is seen by the next pass,
/// not by this one.
///
/// <see cref="ForEachTarget" /> also records which target each thread is calling, so that
/// <see cref="RemoveAndWait" /> can wait until no OTHER thread is inside a call to the target it removes: once it
/// returns, no pass will call that target again, and it can be destroyed. <see cref="Remove" /> never waits, because
/// removing a target is also how it is muted - removed and added back around a change - sometimes while holding a
/// lock its own handler takes. Two rules follow for RemoveAndWait. A target's handler must never wait on a thread
/// that may be removing and waiting for that same target. And nothing may call RemoveAndWait while holding a lock
/// that the target's handler takes.
/// </remarks>
template<class TNotifyTarget>
class NotifyTargetSet
{
private:
    using TargetList = std::vector<TNotifyTarget*>;
    using SharedTargetList = std::shared_ptr<const TargetList>;

public:
    // std::condition_variable's constructor is not noexcept, but IGameContext and IEmulatorMemoryContext declare
    // noexcept default constructors around a set.
    GSL_SUPPRESS_F6 NotifyTargetSet() noexcept = default;
    ~NotifyTargetSet() noexcept = default;

    NotifyTargetSet(const NotifyTargetSet&) = delete;
    NotifyTargetSet& operator=(const NotifyTargetSet&) = delete;

    // Moves the targets, as the old node-based set did. Not while the set is in use: calls in progress are not
    // carried over.
    GSL_SUPPRESS_F6 NotifyTargetSet(NotifyTargetSet&& other) noexcept
    {
        std::lock_guard<std::mutex> lock(other.m_mtxTargets);
        m_pTargets = std::move(other.m_pTargets);
    }

    GSL_SUPPRESS_F6 NotifyTargetSet& operator=(NotifyTargetSet&& other) noexcept
    {
        if (this != &other)
        {
            std::scoped_lock lock(m_mtxTargets, other.m_mtxTargets);
            m_pTargets = std::move(other.m_pTargets);
        }

        return *this;
    }

    class ValidTargets
    {
    public:
        explicit ValidTargets(SharedTargetList pTargets) noexcept : m_pTargets(std::move(pTargets))
        {
        }
        ~ValidTargets() noexcept = default;

        ValidTargets(const ValidTargets&) noexcept = default;
        ValidTargets& operator=(const ValidTargets&) noexcept = default;
        ValidTargets(ValidTargets&&) noexcept = default;
        ValidTargets& operator=(ValidTargets&&) noexcept = default;

        class const_iterator : public std::iterator_traits<typename TargetList::const_iterator>
        {
        public:
            TNotifyTarget& operator*() noexcept { return *(*m_pCurrent); }

            const_iterator& operator++() noexcept
            {
                ++m_pCurrent;
                return *this;
            }

            const_iterator operator++(int)
            {
                auto pBeforeIncrement = *this;
                m_pCurrent++;
                return pBeforeIncrement;
            }

            bool operator==(const const_iterator& that) const noexcept
            {
                return m_pCurrent == that.m_pCurrent;
            }

            bool operator!=(const const_iterator& that) const noexcept
            {
                return !(*this == that);
            }

        private:
            friend class ValidTargets;
            const_iterator(typename TargetList::const_iterator pCurrent) noexcept
                : m_pCurrent(std::move(pCurrent))
            {
            }

            typename TargetList::const_iterator m_pCurrent;
        };

        auto begin() const noexcept { return const_iterator(m_pTargets->cbegin()); }
        auto end() const noexcept { return const_iterator(m_pTargets->cend()); }

        size_t size() const noexcept { return m_pTargets->size(); }

    private:
        // Owned, so the list stays alive - and unchanged - for as long as this object does. Never null, except in a
        // moved-from object.
        SharedTargetList m_pTargets;
    };

    /// <summary>
    /// Gets the objects in the collection.
    /// </summary>
    /// <remarks>
    /// The result owns the list that was current when it was taken, so changes made after that - on any thread - do
    /// not affect it. Unlike <see cref="ForEachTarget" />, it does not make <see cref="RemoveAndWait" /> wait for a target
    /// it yielded that is still being called.
    /// </remarks>
    GSL_SUPPRESS_F6 const ValidTargets Targets() const noexcept
    {
        std::lock_guard<std::mutex> lock(m_mtxTargets);
        return ValidTargets(m_pTargets ? m_pTargets : EmptyList());
    }

    /// <summary>
    /// Calls <paramref name="fCall" /> with each object in the collection, as <see cref="Targets" /> would yield
    /// them - except that an object removed since the pass began is skipped, and <see cref="RemoveAndWait" /> on another
    /// thread waits for a call in progress to return.
    /// </summary>
    template<typename TCall>
    void ForEachTarget(TCall&& fCall)
    {
        SharedTargetList pTargets;
        {
            std::lock_guard<std::mutex> lock(m_mtxTargets);
            if (!m_pTargets || m_pTargets->empty())
                return;

            pTargets = m_pTargets;
        }

        const auto nThreadId = std::this_thread::get_id();
        // Never null: Add takes a reference, so nothing else is ever listed.
        GSL_SUPPRESS_F23 for (TNotifyTarget* pTarget : *pTargets)
        {
            {
                std::lock_guard<std::mutex> lock(m_mtxTargets);
                if (!Contains(pTarget))
                    continue; // removed since the pass began: it may already be gone

                m_vCallsInProgress.push_back({ pTarget, nThreadId });
            }

            const CallInProgress oCall(*this, pTarget, nThreadId);
            fCall(*pTarget);
        }
    }

    /// <summary>
    /// Adds an object reference to the collection.
    /// </summary>
    GSL_SUPPRESS_F6 // this should only throw an exception if we're out of memory
    void Add(TNotifyTarget& pTarget) noexcept
    {
        std::lock_guard<std::mutex> lock(m_mtxTargets);
        if (!m_pTargets)
        {
            m_pTargets = std::make_shared<TargetList>(TargetList{ &pTarget });
            return;
        }

        if (std::find(m_pTargets->begin(), m_pTargets->end(), &pTarget) != m_pTargets->end())
            return;

        auto pNewTargets = std::make_shared<TargetList>(*m_pTargets);
        pNewTargets->push_back(&pTarget);
        m_pTargets = std::move(pNewTargets);
    }

    /// <summary>
    /// Removes an object reference from the collection.
    /// </summary>
    /// <remarks>
    /// Does not wait: a <see cref="ForEachTarget" /> call to it already running on another thread may still be running
    /// when this returns, though no pass will start a new one. Right for muting a target that stays alive; before
    /// destroying a target, use <see cref="RemoveAndWait" />.
    /// </remarks>
    GSL_SUPPRESS_F6 // only a mutex or an allocation failure can throw here
    GSL_SUPPRESS_CON3 // non-const, as Add's is: a const one would only move the warning into every caller's wrapper
    void Remove(TNotifyTarget& pTarget) noexcept
    {
        std::lock_guard<std::mutex> lock(m_mtxTargets);
        Unpublish(pTarget);
    }

    /// <summary>
    /// Removes an object reference from the collection, then waits until no other thread is inside a
    /// <see cref="ForEachTarget" /> call to it - whether or not it was still in the collection. Use it before
    /// destroying the target.
    /// </summary>
    /// <remarks>
    /// A call on this thread, such as a handler removing itself, is not waited for; but a handler that calls
    /// RemoveAndWait still waits for any OTHER thread inside that target. So two threads inside the same target that
    /// each remove it this way deadlock, as do two handlers on two threads that remove each other's targets. Call it
    /// before the target's destruction begins: from a base-class destructor it is too late, because a call on another
    /// thread may be inside a derived override.
    /// </remarks>
    GSL_SUPPRESS_F6 // only a mutex or an allocation failure can throw here
    GSL_SUPPRESS_CON3 // non-const, as Add's is: a const one would only move the warning into every caller's wrapper
    void RemoveAndWait(TNotifyTarget& pTarget) noexcept
    {
        std::unique_lock<std::mutex> lock(m_mtxTargets);
        Unpublish(pTarget);

        const auto nThreadId = std::this_thread::get_id();
        m_cvCallEnded.wait(lock, [this, &pTarget, nThreadId]() { return !IsCalledElsewhere(&pTarget, nThreadId); });
    }

    /// <summary>
    /// Removes all objects from the collection.
    /// </summary>
    /// <remarks>
    /// Unlike <see cref="RemoveAndWait" />, does not wait for calls in progress.
    /// </remarks>
    GSL_SUPPRESS_F6 void Clear() noexcept
    {
        std::lock_guard<std::mutex> lock(m_mtxTargets);
        m_pTargets.reset();
    }

    /// <summary>
    /// Gets whether the collection contains no items.
    /// </summary>
    GSL_SUPPRESS_F6 bool IsEmpty() const noexcept
    {
        std::lock_guard<std::mutex> lock(m_mtxTargets);
        return !m_pTargets || m_pTargets->empty();
    }

    /// <summary>
    /// Kept for source compatibility. A pass no longer needs a lock: <see cref="Targets" /> returns a list that
    /// nothing changes.
    /// </summary>
    /// <returns><c>true</c> if the collection is not empty.</returns>
    bool LockIfNotEmpty() noexcept { return !IsEmpty(); }

    /// <summary>Kept for source compatibility; does nothing (see <see cref="LockIfNotEmpty" />).</summary>
    void Lock() noexcept {}

    /// <summary>Kept for source compatibility; does nothing (see <see cref="LockIfNotEmpty" />).</summary>
    void Unlock() noexcept {}

private:
    struct CallRecord
    {
        const TNotifyTarget* pTarget = nullptr;
        std::thread::id nThreadId;
    };

    // Ends a call ForEachTarget recorded - also when the handler throws - and wakes any RemoveAndWait waiting for it.
    class CallInProgress
    {
    public:
        CallInProgress(NotifyTargetSet& pOwner, const TNotifyTarget* pTarget, std::thread::id nThreadId) noexcept
            : m_pOwner(pOwner), m_pTarget(pTarget), m_nThreadId(nThreadId)
        {
        }
        ~CallInProgress() noexcept { m_pOwner.EndCall(m_pTarget, m_nThreadId); }

        CallInProgress(const CallInProgress&) noexcept = delete;
        CallInProgress& operator=(const CallInProgress&) noexcept = delete;
        CallInProgress(CallInProgress&&) noexcept = delete;
        CallInProgress& operator=(CallInProgress&&) noexcept = delete;

    private:
        NotifyTargetSet& m_pOwner;
        const TNotifyTarget* m_pTarget;
        std::thread::id m_nThreadId;
    };

    GSL_SUPPRESS_F6 void EndCall(const TNotifyTarget* pTarget, std::thread::id nThreadId) noexcept
    {
        {
            std::lock_guard<std::mutex> lock(m_mtxTargets);
            const auto pIter = std::find_if(m_vCallsInProgress.begin(), m_vCallsInProgress.end(),
                [pTarget, nThreadId](const CallRecord& oCall) {
                    return oCall.pTarget == pTarget && oCall.nThreadId == nThreadId;
                });
            if (pIter != m_vCallsInProgress.end())
                m_vCallsInProgress.erase(pIter);
        }

        m_cvCallEnded.notify_all();
    }

    // Publishes a list without pTarget, if it is listed. m_mtxTargets must be held.
    GSL_SUPPRESS_F6 void Unpublish(const TNotifyTarget& pTarget) noexcept
    {
        if (!m_pTargets)
            return;

        const auto pIter = std::find(m_pTargets->begin(), m_pTargets->end(), &pTarget);
        if (pIter == m_pTargets->end())
            return;

        auto pNewTargets = std::make_shared<TargetList>(*m_pTargets);
        pNewTargets->erase(pNewTargets->begin() + (pIter - m_pTargets->begin()));
        m_pTargets = std::move(pNewTargets);
    }

    // m_mtxTargets must be held.
    GSL_SUPPRESS_F6 bool Contains(const TNotifyTarget* pTarget) const noexcept
    {
        return m_pTargets && std::find(m_pTargets->begin(), m_pTargets->end(), pTarget) != m_pTargets->end();
    }

    // m_mtxTargets must be held.
    GSL_SUPPRESS_F6 bool IsCalledElsewhere(const TNotifyTarget* pTarget, std::thread::id nThreadId) const noexcept
    {
        return std::any_of(m_vCallsInProgress.begin(), m_vCallsInProgress.end(),
            [pTarget, nThreadId](const CallRecord& oCall) {
                return oCall.pTarget == pTarget && oCall.nThreadId != nThreadId;
            });
    }

    GSL_SUPPRESS_F6 static const SharedTargetList& EmptyList() noexcept
    {
        static const SharedTargetList pEmpty = std::make_shared<TargetList>();
        return pEmpty;
    }

    mutable std::mutex m_mtxTargets;
    SharedTargetList m_pTargets;                  // null when empty; a published list is never changed
    std::vector<CallRecord> m_vCallsInProgress;   // ForEachTarget's calls running now, on any thread
    std::condition_variable m_cvCallEnded;
};

} // namespace data
} // namespace ra

#endif RA_DATA_NOTIFYTARGET_SET_H
