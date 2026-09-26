#ifndef RA_SERVICES_QT_AUDIOSYSTEM_HH
#define RA_SERVICES_QT_AUDIOSYSTEM_HH
#pragma once

#include "services/IAudioSystem.hh"
#include "services/IQtApplicationHost.hh"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

class QSoundEffect;

namespace ra {
namespace services {
namespace impl {

class QtAudioSystem : public ra::services::IAudioSystem
{
public:
    explicit QtAudioSystem(ra::services::IQtApplicationHost& pHost);
    ~QtAudioSystem() noexcept;

    void PlayAudioFile(const std::wstring& sPath) const override;
    void Beep() const override;

    /// <summary>
    /// The number of effects currently held in the pool. Exposed for the tests only.
    /// </summary>
    size_t PooledEffectCount() const;

private:
    // Holds the effects PlayAudioFile creates, and the mutex guarding them.
    // This lives behind a shared_ptr rather than as plain members so it can
    // outlive *this*: PlayAudioFile hands a copy of the shared_ptr to the
    // QMetaObject::invokeMethod call it queues onto the application thread,
    // and each effect's own finish handler (see the .cpp) holds another
    // copy. If the audio system were ever replaced via
    // ServiceLocator::Provide while a call is still queued or a sound is
    // still playing, the pool stays alive until the last reference drops
    // it, instead of leaving those callbacks pointing at a freed
    // QtAudioSystem. No member here needs `mutable`: PlayAudioFile only
    // ever copies the shared_ptr, which is a const operation.
    struct EffectPool
    {
        std::mutex m_oMutex;
        std::vector<std::unique_ptr<QSoundEffect>> m_vEffects;
    };

    // Each effect reaps itself from the pool via its own playingChanged/
    // statusChanged handlers (connected without moc - see the .cpp) once it
    // can no longer be playing: either it finished after actually starting,
    // or its source failed to load. isPlaying() alone cannot tell those
    // apart from "still loading, hasn't started yet", which is why this is
    // not a simple predicate swept over the pool. Reaping takes the effect
    // out of the pool and deleteLater()s it rather than destroying it, since
    // the handler runs inside the effect's own signal emission. An effect
    // whose source never reaches a terminal status at all - a hang or
    // backend bug rather than a clean finish or Error - is never reaped;
    // that residual leak cannot be detected from out here.
    ra::services::IQtApplicationHost& m_pHost;
    mutable std::atomic<bool> m_bReportedUnavailable{false};
    std::shared_ptr<EffectPool> m_pPool;
};

} // namespace impl
} // namespace services
} // namespace ra

#endif // !RA_SERVICES_QT_AUDIOSYSTEM_HH
