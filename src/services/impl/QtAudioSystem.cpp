#include "QtAudioSystem.hh"

#include "services/IFileSystem.hh"
#include "services/ServiceLocator.hh"

#include "util/Log.hh"

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QAudioDevice>
#include <QAudioSink>
#include <QBuffer>
#include <QMediaDevices>
#include <QUrl>

#include <algorithm>
#include <cstdio>

namespace ra {
namespace services {
namespace impl {

namespace {

// Removes pObject - a sink or a decoder - from its pool. Called from the
// object's own terminal handler (connected in PlayDecoded/StartDecoding),
// never from a bulk sweep - see the header for why. Takes the mutex/vector
// directly rather than QtAudioSystem::EffectPool so it can sit outside the
// class without needing access to that private nested type.
//
// It runs inside pObject's own signal emission, so it must not destroy
// pObject: the emitting code may still touch the object after the handler
// returns. So the pool only gives up ownership under the lock, and
// deleteLater() destroys the object from the event loop once the emission has
// unwound. Only the call that finds pObject in the pool schedules the delete,
// so a second reap finds nothing and does nothing.
template<typename TObject>
void ReapFromPool(std::mutex& oMutex, std::vector<std::unique_ptr<TObject>>& vObjects, TObject* pObject)
{
    TObject* pReaped = nullptr;
    {
        std::scoped_lock<std::mutex> oLock(oMutex);
        const auto pIter = std::find_if(vObjects.begin(), vObjects.end(),
                                        [pObject](const std::unique_ptr<TObject>& p) { return p.get() == pObject; });
        if (pIter == vObjects.end())
            return;

        pReaped = pIter->release();
        vObjects.erase(pIter);
    }

    // The object's connections hold this library's lambdas (and what they
    // capture), and they would go only with the object. Borrowed, its
    // DeferredDelete waits in the host's queue, which a loader's dlclose can
    // outlive; the destructor would then destroy those lambdas through
    // unmapped code. Dropped here, they go now - after the emission that
    // called us returns, as Qt keeps a slot alive while it runs - with the
    // library still loaded. Every one of them was connected with the object
    // itself as its context, so naming it as the receiver drops exactly those
    // and leaves anything Qt connected to the object's signals alone.
    QObject::disconnect(pReaped, nullptr, pReaped, nullptr);
    pReaped->deleteLater();
}

// A sink that cannot play - no audio device, a format the device refuses, a
// stream that broke - would otherwise go without a word: the log would show
// neither "Playing sound" nor a warning, which is how a silent chime becomes
// undiagnosable. Every later chime would most likely fail the same way, so
// only the first failure in the process is reported. Only the Qt thread calls
// this; the flag is atomic because nothing else guards it. The unit tests
// cannot see the warning: they build with RA_UTEST, which compiles every
// RA_LOG_* call to nothing.
std::atomic<bool> g_bReportedSinkFailure{false};

void ReportSinkFailure(const QString& sPath, QtAudio::Error nError)
{
    if (g_bReportedSinkFailure.exchange(true))
        return;

    const char* sName = "";
    switch (nError)
    {
        case QtAudio::OpenError:
            sName = " (OpenError)";
            break;
        case QtAudio::IOError:
            sName = " (IOError)";
            break;
        case QtAudio::FatalError:
            sName = " (FatalError)";
            break;
        default:
            break;
    }

    RA_LOG_WARN("Could not play %s: audio output error %d%s", sPath.toStdString().c_str(),
                static_cast<int>(nError), sName);
}

} // namespace

QtAudioSystem::QtAudioSystem(ra::services::IQtApplicationHost& pHost)
    : m_pHost(pHost), m_pPool(std::make_shared<EffectPool>())
{
    // Every pooled sink and decoder is a QObject on the Qt thread, and must be
    // gone before the application is. The hook holds the pool, not this
    // object, for the reason m_pPool is a shared_ptr (see the header). It
    // destroys them directly: it runs on the Qt thread, but outside any of
    // their own signals, which is the case ReapFromPool has to avoid. The
    // decoded sounds stay: they are plain data.
    std::shared_ptr<EffectPool> pPool = m_pPool;
    m_pHost.AddStopHook([pPool]() {
        std::vector<std::unique_ptr<QAudioSink>> vSinks;
        std::vector<std::unique_ptr<QAudioDecoder>> vDecoders;
        {
            std::scoped_lock<std::mutex> oLock(pPool->m_oMutex);
            vSinks.swap(pPool->m_vSinks);
            vDecoders.swap(pPool->m_vDecoders);
        }

        // The plays waiting on a decode go with its decoder.
        pPool->m_mPendingPlays.clear();

        // Each object's handlers are dropped before it is stopped: on Qt
        // 6.11's FFmpeg backend QAudioDecoder::stop() reports finished()
        // synchronously, and the handler would then cache a partial decode and
        // start sinks after the pool was emptied.
        for (auto& pSink : vSinks)
        {
            QObject::disconnect(pSink.get(), nullptr, pSink.get(), nullptr);
            pSink->stop();
        }
        for (auto& pDecoder : vDecoders)
        {
            QObject::disconnect(pDecoder.get(), nullptr, pDecoder.get(), nullptr);
            pDecoder->stop();
        }
    });
}
QtAudioSystem::~QtAudioSystem() noexcept = default;

void QtAudioSystem::PlayAudioFile(const std::wstring& sPath) const
{
    if (!m_pHost.IsAvailable())
    {
        if (!m_bReportedUnavailable.exchange(true))
            RA_LOG_WARN("PlayAudioFile ignored: Qt services are unavailable");
        return;
    }

    const auto& pFileSystem = ra::services::ServiceLocator::Get<ra::services::IFileSystem>();
    const std::wstring sFullPath = pFileSystem.BaseDirectory() + sPath;

    // AchievementRuntime plays unlock sounds from the frame/worker thread, but
    // the decoder and the sink are QObjects whose signals the Qt thread's event
    // loop delivers, and the pool's maps belong to that thread; m_pHost.Invoke
    // runs this there - inline when already on it.
    const QString sQtPath = QString::fromStdWString(sFullPath);

    // Captured by value instead of `this`: the pool must be able to outlive
    // this object (see the comment on m_pPool in the header), so the queued
    // call only ever reaches it through its own shared_ptr reference, never
    // back through the QtAudioSystem that started it.
    std::shared_ptr<EffectPool> pPool = m_pPool;
    m_pHost.Invoke([pPool, sQtPath]() {
        // Reported once, when its decode failed; a missing or broken file is
        // not tried again, and says nothing more.
        if (pPool->m_vFailed.count(sQtPath) != 0)
            return;

        const auto pDecoded = pPool->m_mDecoded.find(sQtPath);
        if (pDecoded != pPool->m_mDecoded.end())
        {
            PlayDecoded(pPool, sQtPath, pDecoded->second);
            return;
        }

        // Already being decoded: it plays once the decode finishes.
        const auto pPending = pPool->m_mPendingPlays.find(sQtPath);
        if (pPending != pPool->m_mPendingPlays.end())
        {
            ++pPending->second;
            return;
        }

        pPool->m_mPendingPlays.emplace(sQtPath, 1);
        StartDecoding(pPool, sQtPath);
    });
}

void QtAudioSystem::PlayDecoded(const std::shared_ptr<EffectPool>& pPool, const QString& sPath,
                                const std::shared_ptr<const DecodedSound>& pSound)
{
    ++pPool->m_nPlaybacksStarted;

    // A QAudioSink over the cached PCM, not a QSoundEffect. Qt 6.11 creates a
    // QSoundEffect's PipeWire stream with media.role=Notification, so a
    // desktop's "notification sounds" mute silences it; a QAudioSink's stream
    // has role Music. On Windows these chimes play in the application's own
    // audio session, which muting system sounds does not touch - they are
    // game audio, not desktop pings. A QMediaPlayer would also be role Music,
    // but runs a whole FFmpeg pipeline (about 28 descriptors and 5 threads)
    // per sound; a sink over PCM decoded once costs a PipeWire stream.
    auto pSink = std::make_unique<QAudioSink>(QMediaDevices::defaultAudioOutput(), pSound->oFormat);
    QAudioSink* const pRawSink = pSink.get();

    // The sink reads through a QBuffer of its own. setData() copies the
    // QByteArray, which is implicitly shared, so no sample is copied, and the
    // buffer only ever reads, so none ever is. As the sink's child it goes
    // with the sink - after ~QAudioSink has stopped reading from it.
    auto* pBuffer = new QBuffer(pRawSink);
    pBuffer->setData(pSound->oPcm);
    pBuffer->open(QIODevice::ReadOnly);

    // Connecting a lambda to another QObject's signal like this needs no moc
    // on QtAudioSystem - only a class that itself declares signals/slots does.
    // It uses the sink itself as its context, which is what ReapFromPool's
    // disconnect relies on.
    auto pHasPlayed = std::make_shared<bool>(false);
    QObject::connect(pRawSink, &QAudioSink::stateChanged, pRawSink,
                     [pPool, pRawSink, pHasPlayed, sPath](QtAudio::State nState) {
                         switch (nState)
                         {
                             case QtAudio::ActiveState:
                                 // loader-smoke's online "login chime started
                                 // playing" check greps RALog.txt for this
                                 // exact string.
                                 if (!*pHasPlayed)
                                 {
                                     *pHasPlayed = true;
                                     RA_LOG_INFO("Playing sound %s", sPath.toStdString().c_str());
                                 }
                                 break;

                             case QtAudio::IdleState:
                                 // The buffer has drained: the sound is over.
                                 // stop() reports StoppedState, with no error,
                                 // back into this handler, which ignores it.
                                 pRawSink->stop();
                                 ReapFromPool(pPool->m_oMutex, pPool->m_vSinks, pRawSink);
                                 break;

                             case QtAudio::StoppedState:
                                 if (pRawSink->error() != QtAudio::NoError)
                                 {
                                     ReportSinkFailure(sPath, pRawSink->error());
                                     ReapFromPool(pPool->m_oMutex, pPool->m_vSinks, pRawSink);
                                 }
                                 break;

                             default:
                                 break;
                         }
                     });

    pRawSink->start(pBuffer);

    // A sink that is stopped already never started, or started and finished,
    // inside start(): with no audio device Qt 6.11 has no backend sink at
    // all, and start() returns in StoppedState with OpenError without
    // reporting anything. Its handler then ran, if at all, against a pool
    // that did not hold it yet, found nothing to reap and scheduled no
    // delete - so let pSink go out of scope here, outside any emission of its
    // signals. A failure its handler saw is reported already, and
    // ReportSinkFailure reports only the first anyway.
    if (pRawSink->state() == QtAudio::StoppedState)
    {
        if (pRawSink->error() != QtAudio::NoError)
            ReportSinkFailure(sPath, pRawSink->error());
        return;
    }

    std::scoped_lock<std::mutex> oLock(pPool->m_oMutex);
    pPool->m_vSinks.push_back(std::move(pSink));
}

void QtAudioSystem::StartDecoding(const std::shared_ptr<EffectPool>& pPool, const QString& sPath)
{
    ++pPool->m_nDecodesStarted;

    auto pDecoder = std::make_unique<QAudioDecoder>();

    // Without a multimedia backend to decode with - Qt's FFmpeg plugin not
    // installed - a decoder reports nothing once started, neither finished()
    // nor error(), and the path would wait in m_mPendingPlays forever. It
    // counts as a failed decode instead, reported once like any other; the
    // decoder was never started, so it goes when this returns. No unit test
    // covers this: it needs Qt's multimedia plugin to be missing.
    if (!pDecoder->isSupported())
    {
        pPool->m_mPendingPlays.erase(sPath);
        if (pPool->m_vFailed.insert(sPath).second)
            RA_LOG_WARN("Could not decode %s: no audio decoder available", sPath.toStdString().c_str());
        return;
    }

    QAudioDecoder* const pRawDecoder = pDecoder.get();

    // Filled buffer by buffer, then cached whole once the decode finishes.
    auto pSound = std::make_shared<DecodedSound>();

    // Every handler uses the decoder itself as its context, which is what
    // ReapFromPool's disconnect relies on.
    QObject::connect(pRawDecoder, &QAudioDecoder::bufferReady, pRawDecoder, [pRawDecoder, pSound]() {
        const QAudioBuffer oBuffer = pRawDecoder->read();
        if (!oBuffer.isValid())
            return;

        pSound->oFormat = oBuffer.format();
        pSound->oPcm.append(oBuffer.constData<char>(), oBuffer.byteCount());
    });

    QObject::connect(pRawDecoder, &QAudioDecoder::finished, pRawDecoder, [pPool, pRawDecoder, pSound, sPath]() {
        int nPlays = 0;
        const auto pPending = pPool->m_mPendingPlays.find(sPath);
        if (pPending != pPool->m_mPendingPlays.end())
        {
            nPlays = pPending->second;
            pPool->m_mPendingPlays.erase(pPending);
        }

        if (pSound->oPcm.isEmpty() || !pSound->oFormat.isValid())
        {
            // The file opened, but held nothing FFmpeg could decode - a header
            // with no samples, or data it could not make sense of. It
            // finishes without an error, and there is nothing to play.
            if (pPool->m_vFailed.insert(sPath).second)
                RA_LOG_WARN("Could not decode %s: no audio in the file", sPath.toStdString().c_str());
        }
        else
        {
            const std::shared_ptr<const DecodedSound> pDecoded = pSound;
            pPool->m_mDecoded.emplace(sPath, pDecoded);

            for (int i = 0; i < nPlays; ++i)
                PlayDecoded(pPool, sPath, pDecoded);
        }

        ReapFromPool(pPool->m_oMutex, pPool->m_vDecoders, pRawDecoder);
    });

    QObject::connect(pRawDecoder, qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), pRawDecoder,
                     [pPool, pRawDecoder, sPath](QAudioDecoder::Error nError) {
                         if (nError == QAudioDecoder::NoError)
                             return;

                         pPool->m_mPendingPlays.erase(sPath);
                         if (pPool->m_vFailed.insert(sPath).second)
                         {
                             RA_LOG_WARN("Could not decode %s: %s", sPath.toStdString().c_str(),
                                         pRawDecoder->errorString().toStdString().c_str());
                         }

                         ReapFromPool(pPool->m_oMutex, pPool->m_vDecoders, pRawDecoder);
                     });

    pRawDecoder->setSource(QUrl::fromLocalFile(sPath));

    // Decoded straight to the format the default output prefers, so every
    // sink plays it as it is. With no audio device the preferred format is
    // invalid, and the decoder keeps the file's own.
    const QAudioFormat oPreferred = QMediaDevices::defaultAudioOutput().preferredFormat();
    if (oPreferred.isValid())
        pRawDecoder->setAudioFormat(oPreferred);

    pRawDecoder->start();

    // Qt 6.11's FFmpeg backend reports a file it cannot open at all - missing,
    // empty, not audio, no decoder for it - inside start(); a decode that gets
    // going reports finished() later, from the event loop. Either handler takes
    // the path out of m_mPendingPlays, so if it is gone the decoder settled
    // here, against a pool that did not hold it yet: its handler found nothing
    // to reap and scheduled no delete. Let pDecoder go out of scope here,
    // outside any emission of its signals.
    if (pPool->m_mPendingPlays.count(sPath) == 0)
        return;

    std::scoped_lock<std::mutex> oLock(pPool->m_oMutex);
    pPool->m_vDecoders.push_back(std::move(pDecoder));
}

void QtAudioSystem::Beep() const
{
    // QApplication::beep() lives in QtWidgets, which the service layer does not
    // link. Beep() has no callers in src/ outside this interface, so the
    // terminal bell is sufficient until the Qt views phase brings QtWidgets in.
    std::fputc('\a', stderr);
}

size_t QtAudioSystem::PooledEffectCount() const
{
    std::scoped_lock<std::mutex> oLock(m_pPool->m_oMutex);
    return m_pPool->m_vSinks.size() + m_pPool->m_vDecoders.size();
}

size_t QtAudioSystem::DecodeCount() const noexcept { return m_pPool->m_nDecodesStarted.load(); }

size_t QtAudioSystem::PlaybackCount() const noexcept { return m_pPool->m_nPlaybacksStarted.load(); }

} // namespace impl
} // namespace services
} // namespace ra
