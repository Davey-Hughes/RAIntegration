#include "QtAudioSystem.hh"

#include "services/IFileSystem.hh"
#include "services/ServiceLocator.hh"

#include "util/Log.hh"

#include <QGuiApplication>
#include <QMetaObject>
#include <QSoundEffect>
#include <QString>
#include <QUrl>

#include <algorithm>
#include <cstdio>

namespace ra {
namespace services {
namespace impl {

namespace {

// Removes pEffect from the pool. Called from an effect's own
// playingChanged/statusChanged handler (connected in PlayAudioFile) once it
// can no longer be playing, never from a bulk sweep - see the header for why
// isPlaying() alone can't drive that sweep. Takes the mutex/vector directly
// rather than QtAudioSystem::EffectPool so it can sit outside the class
// without needing access to that private nested type.
void ReapEffect(std::mutex& oMutex, std::vector<std::unique_ptr<QSoundEffect>>& vEffects, QSoundEffect* pEffect)
{
    std::scoped_lock<std::mutex> oLock(oMutex);
    vEffects.erase(std::remove_if(vEffects.begin(), vEffects.end(),
                                   [pEffect](const std::unique_ptr<QSoundEffect>& p) { return p.get() == pEffect; }),
                   vEffects.end());
}

} // namespace

QtAudioSystem::QtAudioSystem() noexcept : m_pPool(std::make_shared<EffectPool>()) {}
QtAudioSystem::~QtAudioSystem() noexcept = default;

void QtAudioSystem::PlayAudioFile(const std::wstring& sPath) const
{
    auto* pApplication = QGuiApplication::instance();
    if (pApplication == nullptr)
    {
        RA_LOG_WARN("PlayAudioFile ignored: no QGuiApplication");
        return;
    }

    const auto& pFileSystem = ra::services::ServiceLocator::Get<ra::services::IFileSystem>();
    const std::wstring sFullPath = pFileSystem.BaseDirectory() + sPath;

    // AchievementRuntime plays unlock sounds from the frame/worker thread, but
    // a QSoundEffect must be created on the thread owning the event loop.
    const QString sQtPath = QString::fromStdWString(sFullPath);

    // Captured by value instead of `this`: the pool must be able to outlive
    // this object (see the comment on m_pPool in the header), so the queued
    // call only ever reaches it through its own shared_ptr reference, never
    // back through the QtAudioSystem that started it.
    std::shared_ptr<EffectPool> pPool = m_pPool;
    QMetaObject::invokeMethod(
        pApplication,
        [pPool, sQtPath]() {
            auto pEffect = std::make_unique<QSoundEffect>();
            QSoundEffect* const pRawEffect = pEffect.get();

            // isPlaying() alone cannot distinguish "finished" from "still
            // loading, hasn't started yet": immediately after play() on an
            // unloaded source, isPlaying() is false and status() is
            // Loading, and it only becomes true asynchronously once the
            // backend finishes decoding. So track whether this effect has
            // actually been observed playing, and only reap it once it
            // stops *after* that. Connecting a lambda to another QObject's
            // signal like this needs no moc on QtAudioSystem - only a class
            // that itself declares signals/slots does.
            auto pHasPlayed = std::make_shared<bool>(false);
            QObject::connect(pRawEffect, &QSoundEffect::playingChanged, pRawEffect,
                              [pPool, pRawEffect, pHasPlayed]() {
                                  if (pRawEffect->isPlaying())
                                  {
                                      *pHasPlayed = true;
                                      return;
                                  }

                                  if (*pHasPlayed)
                                      ReapEffect(pPool->m_oMutex, pPool->m_vEffects, pRawEffect);
                              });

            // A source that fails to load (bad path, unsupported format,
            // etc.) never starts playing, so playingChanged above never
            // fires for it; reap it here instead so it isn't leaked.
            QObject::connect(pRawEffect, &QSoundEffect::statusChanged, pRawEffect, [pPool, pRawEffect]() {
                if (pRawEffect->status() == QSoundEffect::Error && !pRawEffect->isPlaying())
                    ReapEffect(pPool->m_oMutex, pPool->m_vEffects, pRawEffect);
            });

            pEffect->setSource(QUrl::fromLocalFile(sQtPath));
            pEffect->setLoopCount(1);
            pEffect->play();

            // setSource()/play() can settle synchronously for some backends
            // or sources (an immediate load failure, or a cached/zero-length
            // sound that starts and finishes before either call returns).
            // The handlers above would have already run against an empty
            // pool in that case, so check the now-final state directly
            // instead of relying on a signal that already fired.
            if (pEffect->status() == QSoundEffect::Error)
                return;
            if (!pEffect->isPlaying() && *pHasPlayed)
                return;

            std::scoped_lock<std::mutex> oLock(pPool->m_oMutex);
            pPool->m_vEffects.push_back(std::move(pEffect));
        },
        Qt::QueuedConnection);
}

void QtAudioSystem::Beep() const
{
    // QApplication::beep() lives in QtWidgets, which the service layer does not
    // link. Beep() has no callers in src/ outside this interface, so the
    // terminal bell is sufficient until the Qt views phase brings QtWidgets in.
    std::fputc('\a', stderr);
}

} // namespace impl
} // namespace services
} // namespace ra
