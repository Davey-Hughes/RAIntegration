#include "QtAudioSystem.hh"

#include "services/IFileSystem.hh"
#include "services/ServiceLocator.hh"

#include "util/Log.hh"
#include "util/Strings.hh"

#include <QGuiApplication>
#include <QMetaObject>
#include <QSoundEffect>
#include <QString>
#include <QUrl>

#include <cstdio>

namespace ra {
namespace services {
namespace impl {

QtAudioSystem::QtAudioSystem() noexcept = default;
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
    QMetaObject::invokeMethod(
        pApplication,
        [this, sQtPath]() {
            std::scoped_lock<std::mutex> oLock(m_oMutex);

            // reap anything that has finished since the last call
            for (auto pIterator = m_vEffects.begin(); pIterator != m_vEffects.end();)
            {
                if (!(*pIterator)->isPlaying())
                    pIterator = m_vEffects.erase(pIterator);
                else
                    ++pIterator;
            }

            auto pEffect = std::make_unique<QSoundEffect>();
            pEffect->setSource(QUrl::fromLocalFile(sQtPath));
            pEffect->setLoopCount(1);
            pEffect->play();
            m_vEffects.push_back(std::move(pEffect));
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
