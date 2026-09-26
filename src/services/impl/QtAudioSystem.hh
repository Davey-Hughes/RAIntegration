#ifndef RA_SERVICES_QT_AUDIOSYSTEM_HH
#define RA_SERVICES_QT_AUDIOSYSTEM_HH
#pragma once

#include "services/IAudioSystem.hh"
#include "services/IQtApplicationHost.hh"

#include <QAudioFormat>
#include <QByteArray>
#include <QString>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

class QAudioDecoder;
class QAudioSink;

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
    /// Test seam: the number of objects currently held in the pool - sinks still playing plus decoders still
    /// decoding. Callable from any thread.
    /// </summary>
    size_t PooledEffectCount() const;

    /// <summary>
    /// Test seam: the number of decoders started so far, whether they succeeded or not. Each sound file is decoded
    /// once, so this grows only on the first play of a path.
    /// </summary>
    size_t DecodeCount() const noexcept;

    /// <summary>
    /// Test seam: the number of sinks created so far - one per play of a decoded sound, whether or not an audio
    /// device then let it start.
    /// </summary>
    size_t PlaybackCount() const noexcept;

private:
    // A sound file decoded to PCM, in the format the decoder delivered it in.
    struct DecodedSound
    {
        QAudioFormat oFormat;
        QByteArray oPcm;
    };

    // Everything PlayAudioFile creates and remembers. It lives behind a
    // shared_ptr rather than as plain members so it can outlive *this*:
    // PlayAudioFile passes a lambda to m_pHost.Invoke, which runs it inline
    // when already on the Qt thread and queues it otherwise, and that lambda
    // holds a copy of m_pPool. The constructor also registers a stop hook that
    // holds another copy, and every pooled object's own handlers (see the
    // .cpp) hold another. If the audio system were ever replaced via
    // ServiceLocator::Provide while a call is still queued or a sound is still
    // playing, the pool stays alive until the last reference drops it,
    // instead of leaving those callbacks pointing at a freed QtAudioSystem. No
    // member here needs `mutable`: PlayAudioFile only ever copies the
    // shared_ptr, which is a const operation.
    struct EffectPool
    {
        // The Qt objects in flight: a QAudioSink per sound playing, and a
        // QAudioDecoder per sound file still being decoded. Only the Qt thread
        // adds or removes them; the mutex is there for PooledEffectCount(),
        // which any thread may call.
        std::mutex m_oMutex;
        std::vector<std::unique_ptr<QAudioSink>> m_vSinks;
        std::vector<std::unique_ptr<QAudioDecoder>> m_vDecoders;

        // Everything below is read and written on the Qt thread only, so it
        // takes no lock. Each is keyed by the sound's full path.
        //
        // m_mDecoded: every file decoded so far, played from here ever after.
        //   Plain data, so it may outlive the application.
        // m_mPendingPlays: the files being decoded right now, and how many
        //   plays were asked for each meanwhile - a burst of identical unlocks
        //   arrives before the first decode finishes, and all of it must play.
        // m_vFailed: the files whose decode failed. They are reported once and
        //   never tried again, so a missing file does not spam the log.
        std::map<QString, std::shared_ptr<const DecodedSound>> m_mDecoded;
        std::map<QString, int> m_mPendingPlays;
        std::set<QString> m_vFailed;

        // The test seams' counters, atomic so a test may read them from any
        // thread.
        std::atomic<size_t> m_nDecodesStarted{0};
        std::atomic<size_t> m_nPlaybacksStarted{0};
    };

    // Both run on the Qt thread. PlayDecoded plays pSound once through a new
    // sink; StartDecoding decodes sPath into m_mDecoded and then plays it as
    // many times as m_mPendingPlays asked for meanwhile.
    static void PlayDecoded(const std::shared_ptr<EffectPool>& pPool, const QString& sPath,
                            const std::shared_ptr<const DecodedSound>& pSound);
    static void StartDecoding(const std::shared_ptr<EffectPool>& pPool, const QString& sPath);

    // Each pooled object reaps itself - out of the pool, then deleteLater() -
    // from its own terminal signal: a decoder on finished() or error(), a sink
    // once its buffer has drained (IdleState) or it stopped on an error. Those
    // are states the object reports itself, rather than a predicate swept over
    // the pool, because an object that is not active yet may only be
    // starting. It is deleted later rather than destroyed because the handler
    // runs inside the object's own signal emission. An object that never
    // reaches a terminal state - a hang or backend bug - is never reaped; that
    // residual leak cannot be detected from out here, and the stop hook still
    // destroys it.
    ra::services::IQtApplicationHost& m_pHost;
    mutable std::atomic<bool> m_bReportedUnavailable{false};
    std::shared_ptr<EffectPool> m_pPool;
};

} // namespace impl
} // namespace services
} // namespace ra

#endif // !RA_SERVICES_QT_AUDIOSYSTEM_HH
