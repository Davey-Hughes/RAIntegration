#ifndef RA_SERVICES_QT_AUDIOSYSTEM_HH
#define RA_SERVICES_QT_AUDIOSYSTEM_HH
#pragma once

#include "services/IAudioSystem.hh"

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
    QtAudioSystem() noexcept;
    ~QtAudioSystem() noexcept;

    void PlayAudioFile(const std::wstring& sPath) const override;
    void Beep() const override;

private:
    // Effects must outlive the call that started them. Finished ones are
    // reaped on the next call rather than through playingChanged, which keeps
    // this class out of the moc and avoids mutating the list from a signal.
    mutable std::mutex m_oMutex;
    mutable std::vector<std::unique_ptr<QSoundEffect>> m_vEffects;
};

} // namespace impl
} // namespace services
} // namespace ra

#endif // !RA_SERVICES_QT_AUDIOSYSTEM_HH
