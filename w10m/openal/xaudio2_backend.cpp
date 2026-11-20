/* OpenAL Soft playback backend over XAudio2, for Windows 10 Mobile.
 *
 * OpenAL Soft's own UWP backend is WASAPI through ActivateAudioInterfaceAsync, which faults in
 * MMDevAPI.dll on 15063 phones when it is called from a thread that is not the UI thread (seen
 * with miniaudio in butterscotch-w10m). XAudio2 has no such requirement and is what that port
 * and LibertyRecomp play through on the same phones.
 *
 * The mixer thread renders one update at a time into a small ring of blocks and keeps a few of
 * them queued on one source voice; XAudio2's own thread only signals. XAudio2 does not copy a
 * submitted block, so the ring has one block more than may be queued.
 *
 * XAudio2 is loaded at run time (2.9, Windows 10's, then 2.8, which every Windows 10 Mobile
 * build also has): nothing here is a load-time import, and a phone without either gets the
 * next backend in the list instead of a package that does not start.
 *
 * After OnCriticalError or OnVoiceError (the endpoint went away: headphones, a Bluetooth
 * headset, the audio service) an XAudio2 engine stays dead. The mixer thread then releases it
 * and makes a new one in the same source format; until that works the mix is rendered and
 * dropped at the pace it would have played, so the game's sounds keep their timing. */
#include "config.h"

#include "xaudio2_backend.h"

#include <windows.h>
#include <objbase.h>
#include <xaudio2.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <thread>
#include <vector>

#include "althrd_setname.h"
#include "core/device.h"
#include "core/helpers.h"
#include "core/logging.h"

namespace {

using namespace std::string_view_literals;

[[nodiscard]] constexpr auto GetDeviceName() noexcept { return "XAudio2 Default"sv; }

/* Blocks kept queued on the voice: the latency is this many updates. */
constexpr uint QueuedBlocks{3};
constexpr DWORD RetryMs{1000};

using XAudio2CreateFn = HRESULT (WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);

HMODULE gXAudio2Module{};
XAudio2CreateFn gXAudio2Create{};

bool LoadXAudio2()
{
    if(gXAudio2Create)
        return true;
#ifdef REVC_WP81
    /* Windows Phone 8.1 has 2.8 alone, and this build's xaudio2.h is 2.8's (_WIN32_WINNT 8.1) */
    for(const WCHAR *name : {L"XAudio2_8.dll", L"XAudio2_9.dll"})
#else
    for(const WCHAR *name : {L"XAudio2_9.dll", L"XAudio2_8.dll"})
#endif
    {
        HMODULE module{LoadLibraryExW(name, nullptr, 0)};
        if(!module)
            continue;
        auto create = reinterpret_cast<XAudio2CreateFn>(
            reinterpret_cast<void*>(GetProcAddress(module, "XAudio2Create")));
        if(create)
        {
            gXAudio2Module = module;
            gXAudio2Create = create;
            TRACE("XAudio2 from {}", name == std::wstring_view{L"XAudio2_9.dll"} ? "XAudio2_9.dll"
                : "XAudio2_8.dll");
            return true;
        }
        FreeLibrary(module);
    }
    WARN("Neither XAudio2_9.dll nor XAudio2_8.dll loads (error {})", GetLastError());
    return false;
}


struct XAudio2Backend final : public BackendBase, IXAudio2VoiceCallback, IXAudio2EngineCallback {
    explicit XAudio2Backend(DeviceBase *device) noexcept : BackendBase{device} { }
    ~XAudio2Backend() override;

    void open(std::string_view name) override;
    bool reset() override;
    void start() override;
    void stop() override;

    HRESULT openEngine();
    HRESULT openVoice();
    void closeEngine();
    int mixerProc();

    /* IXAudio2VoiceCallback */
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override { }
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override { }
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override { }
    void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override { }
    void STDMETHODCALLTYPE OnBufferEnd(void*) noexcept override { SetEvent(mBufferEnd); }
    void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override { }
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT error) noexcept override { lose(error); }
    /* IXAudio2EngineCallback */
    void STDMETHODCALLTYPE OnProcessingPassStart() noexcept override { }
    void STDMETHODCALLTYPE OnProcessingPassEnd() noexcept override { }
    void STDMETHODCALLTYPE OnCriticalError(HRESULT error) noexcept override { lose(error); }

    /* On XAudio2's thread: only flag it and wake the mixer. */
    void lose(HRESULT error) noexcept
    {
        mLostError.store(static_cast<long>(error), std::memory_order_relaxed);
        mLost.store(true, std::memory_order_release);
        SetEvent(mBufferEnd);
    }

    IXAudio2 *mXAudio2{};
    IXAudio2MasteringVoice *mMaster{};
    IXAudio2SourceVoice *mSource{};
    WAVEFORMATEX mFormat{};
    HANDLE mBufferEnd{};

    uint mFrameSize{};
    std::vector<std::byte> mRing; /* QueuedBlocks+1 blocks of mUpdateSize frames */
    uint mBlock{};

    std::atomic<bool> mLost{false};
    std::atomic<long> mLostError{0};
    std::atomic<bool> mKillNow{true};
    std::thread mThread;
};

XAudio2Backend::~XAudio2Backend()
{
    stop();
    closeEngine();
    if(mBufferEnd)
        CloseHandle(mBufferEnd);
}

/* Voices first; Release then ends XAudio2's thread. */
void XAudio2Backend::closeEngine()
{
    if(mSource)
    {
        mSource->DestroyVoice();
        mSource = nullptr;
    }
    if(mMaster)
    {
        mMaster->DestroyVoice();
        mMaster = nullptr;
    }
    if(mXAudio2)
    {
        mXAudio2->UnregisterForCallbacks(this);
        mXAudio2->Release();
        mXAudio2 = nullptr;
    }
}

/* XAudio2Create answers CO_E_NOTINITIALIZED on a thread that is in no apartment. In the app
 * every thread is in the process's multithreaded one, but that is the application's doing, not
 * this library's: each thread that makes an engine here (the one that opens the device, and
 * the mixer when it makes a new one) joins it for as long as the thread lives.
 */
struct ComApartment {
    HRESULT mResult{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    ~ComApartment() { if(SUCCEEDED(mResult)) CoUninitialize(); }
};

/* Engine and mastering voice on the default endpoint, at the endpoint's own rate. */
HRESULT XAudio2Backend::openEngine()
{
    static thread_local ComApartment apartment;
    HRESULT hr{gXAudio2Create(&mXAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR)};
    if(SUCCEEDED(hr))
        hr = mXAudio2->RegisterForCallbacks(this);
    if(SUCCEEDED(hr))
    {
        /* GameEffects mixes with whatever else plays. GameMedia would be muted while the
         * user's own music plays, and the game's effects are in the same stream.
         */
        hr = mXAudio2->CreateMasteringVoice(&mMaster, XAUDIO2_DEFAULT_CHANNELS,
            XAUDIO2_DEFAULT_SAMPLERATE, 0, nullptr, nullptr, AudioCategory_GameEffects);
    }
    if(FAILED(hr))
        closeEngine();
    return hr;
}

/* The source voice in mFormat. XAudio2 converts the rate: a new mastering voice may run at
 * another one (a Bluetooth headset). No pitch changes: the smallest resampler buffers.
 */
HRESULT XAudio2Backend::openVoice()
{
    HRESULT hr{mXAudio2->CreateSourceVoice(&mSource, &mFormat, 0, 1.0f, this, nullptr, nullptr)};
    if(FAILED(hr))
        closeEngine();
    return hr;
}

void XAudio2Backend::open(std::string_view name)
{
    if(name.empty())
        name = GetDeviceName();
    else if(name != GetDeviceName())
        throw al::backend_exception{al::backend_error::NoDevice, "Device name \"{}\" not found",
            name};

    if(!LoadXAudio2())
        throw al::backend_exception{al::backend_error::NoDevice, "XAudio2 is not available"};
    if(!mBufferEnd)
        mBufferEnd = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if(!mBufferEnd)
        throw al::backend_exception{al::backend_error::DeviceError,
            "Failed to create an event: error {}", GetLastError()};

    closeEngine();
    if(const HRESULT hr{openEngine()}; FAILED(hr))
        throw al::backend_exception{al::backend_error::DeviceError,
            "Failed to create XAudio2: {:#010x}", static_cast<unsigned long>(hr)};

    mDeviceName = name;
}

bool XAudio2Backend::reset()
{
    if(mSource)
    {
        mSource->DestroyVoice();
        mSource = nullptr;
    }

    /* Stereo float: what the phone's endpoint is, and what the mixer works in. The rate is the
     * one the application asked for; XAudio2 converts it to the endpoint's.
     */
    mDevice->FmtChans = DevFmtStereo;
    mDevice->FmtType = DevFmtFloat;
    mDevice->mAmbiOrder = 0;
    if(mDevice->mSampleRate < XAUDIO2_MIN_SAMPLE_RATE)
        mDevice->mSampleRate = XAUDIO2_MIN_SAMPLE_RATE;
    if(mDevice->mSampleRate > XAUDIO2_MAX_SAMPLE_RATE)
        mDevice->mSampleRate = XAUDIO2_MAX_SAMPLE_RATE;
    /* An update of about 20 ms: three of them queued are a latency a game can live with, and
     * the phone's audio thread wakes every 10 ms.
     */
    mDevice->mUpdateSize = std::max(mDevice->mSampleRate / 50u, 256u);
    mDevice->mBufferSize = mDevice->mUpdateSize * QueuedBlocks;
    setDefaultWFXChannelOrder();

    mFrameSize = mDevice->frameSizeFromFmt();
    mFormat = WAVEFORMATEX{};
    mFormat.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    mFormat.nChannels = static_cast<WORD>(mDevice->channelsFromFmt());
    mFormat.nSamplesPerSec = mDevice->mSampleRate;
    mFormat.wBitsPerSample = static_cast<WORD>(mDevice->bytesFromFmt() * 8);
    mFormat.nBlockAlign = static_cast<WORD>(mFrameSize);
    mFormat.nAvgBytesPerSec = mFormat.nSamplesPerSec * mFormat.nBlockAlign;

    if(!mXAudio2)
    {
        if(const HRESULT hr{openEngine()}; FAILED(hr))
            throw al::backend_exception{al::backend_error::DeviceError,
                "Failed to create XAudio2: {:#010x}", static_cast<unsigned long>(hr)};
    }
    if(const HRESULT hr{openVoice()}; FAILED(hr))
        throw al::backend_exception{al::backend_error::DeviceError,
            "Failed to create the source voice: {:#010x}", static_cast<unsigned long>(hr)};

    mRing.assign(size_t{mDevice->mUpdateSize} * mFrameSize * (QueuedBlocks+1), std::byte{});
    mBlock = 0;
    return true;
}

int XAudio2Backend::mixerProc()
{
    SetRTPriority();
    althrd_setname(GetMixerThreadName());

    const uint update{mDevice->mUpdateSize};
    const size_t blockBytes{size_t{update} * mFrameSize};
    const auto blockTime = std::chrono::nanoseconds{std::chrono::seconds{1}} * update
        / mDevice->mSampleRate;

    bool down{false};
    auto nextTry = std::chrono::steady_clock::time_point{};
    auto paceNext = std::chrono::steady_clock::time_point{};

    while(!mKillNow.load(std::memory_order_acquire)
        && mDevice->Connected.load(std::memory_order_acquire))
    {
        const auto now = std::chrono::steady_clock::now();
        if(mLost.exchange(false, std::memory_order_acq_rel))
        {
            WARN("XAudio2 lost its endpoint ({:#010x}), making a new engine",
                static_cast<unsigned long>(mLostError.load(std::memory_order_relaxed)));
            closeEngine();
            if(!down)
            {
                down = true;
                nextTry = now;
                paceNext = now;
            }
        }

        if(down)
        {
            if(now >= nextTry)
            {
                HRESULT hr{openEngine()};
                if(SUCCEEDED(hr)) hr = openVoice();
                if(SUCCEEDED(hr))
                {
                    hr = mSource->Start(0, XAUDIO2_COMMIT_NOW);
                    if(FAILED(hr)) closeEngine();
                }
                if(SUCCEEDED(hr))
                {
                    TRACE("XAudio2 is back");
                    down = false;
                    mBlock = 0;
                    continue;
                }
                nextTry = now + std::chrono::milliseconds{RetryMs};
            }
            /* No engine: the mix is made and dropped at the pace it would have played. */
            if(now < paceNext)
            {
                std::this_thread::sleep_for(std::min<std::chrono::nanoseconds>(paceNext - now,
                    std::chrono::milliseconds{20}));
                continue;
            }
            mDevice->renderSamples(mRing.data(), update, mFormat.nChannels);
            paceNext += blockTime;
            if(paceNext + std::chrono::seconds{1} < now)
                paceNext = now; /* suspended in between: do not catch up */
            continue;
        }

        XAUDIO2_VOICE_STATE state{};
        mSource->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if(state.BuffersQueued >= QueuedBlocks)
        {
            /* An update at the latest: a buffer that never comes back (a suspended app) must
             * not hold up stop().
             */
            WaitForSingleObjectEx(mBufferEnd, 100, FALSE);
            continue;
        }

        std::byte *block{mRing.data() + blockBytes*mBlock};
        mDevice->renderSamples(block, update, mFormat.nChannels);

        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = static_cast<UINT32>(blockBytes);
        buffer.pAudioData = reinterpret_cast<const BYTE*>(block);
        if(const HRESULT hr{mSource->SubmitSourceBuffer(&buffer, nullptr)}; FAILED(hr))
        {
            lose(hr);
            continue;
        }
        mBlock = (mBlock+1) % (QueuedBlocks+1);
    }

    return 0;
}

void XAudio2Backend::start()
{
    if(!mSource)
        throw al::backend_exception{al::backend_error::DeviceError, "No source voice"};
    if(const HRESULT hr{mSource->Start(0, XAUDIO2_COMMIT_NOW)}; FAILED(hr))
        throw al::backend_exception{al::backend_error::DeviceError,
            "Failed to start the source voice: {:#010x}", static_cast<unsigned long>(hr)};
    try {
        mKillNow.store(false, std::memory_order_release);
        mThread = std::thread{&XAudio2Backend::mixerProc, this};
    }
    catch(std::exception& e) {
        mKillNow.store(true, std::memory_order_release);
        throw al::backend_exception{al::backend_error::DeviceError,
            "Failed to start mixing thread: {}", e.what()};
    }
}

void XAudio2Backend::stop()
{
    if(mKillNow.exchange(true, std::memory_order_acq_rel) || !mThread.joinable())
        return;
    SetEvent(mBufferEnd);
    mThread.join();

    if(mSource)
    {
        mSource->Stop(0, XAUDIO2_COMMIT_NOW);
        mSource->FlushSourceBuffers();
    }
    mBlock = 0;
}

} // namespace


bool XAudio2BackendFactory::init()
{ return LoadXAudio2(); }

bool XAudio2BackendFactory::querySupport(BackendType type)
{ return type == BackendType::Playback; }

auto XAudio2BackendFactory::enumerate(BackendType type) -> std::vector<std::string>
{
    switch(type)
    {
    case BackendType::Playback:
        return std::vector{std::string{GetDeviceName()}};
    case BackendType::Capture:
        break;
    }
    return {};
}

BackendPtr XAudio2BackendFactory::createBackend(DeviceBase *device, BackendType type)
{
    if(type == BackendType::Playback)
        return BackendPtr{new XAudio2Backend{device}};
    return nullptr;
}

BackendFactory &XAudio2BackendFactory::getFactory()
{
    static XAudio2BackendFactory factory{};
    return factory;
}
