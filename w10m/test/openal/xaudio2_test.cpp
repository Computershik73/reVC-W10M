// Plays through OpenAL Soft's XAudio2 backend (w10m/openal) and checks what can be checked
// without ears: the device is the backend's, a source advances at the rate of the clock, it
// stops at the end of its buffer, and the device can be closed and opened again, as the game
// does while it lists its "providers". The listener's gain is zero: nothing is heard.
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

static int failures;

static void Check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static bool PlayOnce(int round) {
    // The second time by its listed name, which is how the game opens it (oal/aldlist.cpp)
    const ALCchar* listed = round > 1 ? alcGetString(nullptr, ALC_ALL_DEVICES_SPECIFIER) : nullptr;
    if (round > 1) Check(listed && *listed, "the device is listed");
    ALCdevice* device = alcOpenDevice(listed && *listed ? listed : nullptr);
    if (!device) {
        std::printf("FAIL  round %d: no device\n", round);
        failures++;
        return false;
    }
    const ALCchar* name = alcGetString(device, ALC_ALL_DEVICES_SPECIFIER);
    std::printf("      round %d: device \"%s\"\n", round, name ? name : "?");
    // OpenAL Soft puts "OpenAL Soft on " before a backend's device name
    Check(name && std::strstr(name, "XAudio2 Default"), "the device is the XAudio2 backend's");

    // what the game asks for (src/audio/sampman_oal.cpp)
    const ALCint attributes[] = {ALC_FREQUENCY, 44100, ALC_MONO_SOURCES, 32, ALC_STEREO_SOURCES, 4, 0};
    ALCcontext* context = alcCreateContext(device, attributes);
    Check(context != nullptr, "a context at 44100 Hz");
    if (!context) {
        alcCloseDevice(device);
        return false;
    }
    alcMakeContextCurrent(context);
    ALCint rate = 0;
    alcGetIntegerv(device, ALC_FREQUENCY, 1, &rate);
    Check(rate == 44100, "the device runs at the rate asked for");
    Check(alcIsExtensionPresent(device, "ALC_EXT_EFX") == ALC_TRUE, "EFX (the game's reverb) is there");
    const char* extensions = alGetString(AL_EXTENSIONS);
    Check(extensions && std::strstr(extensions, "AL_SOFT_loop_points"), "AL_SOFT_loop_points (the game refuses to start without)");

    alListenerf(AL_GAIN, 0.0f);

    const int kRate = 22050, kFrames = kRate;  // one second
    std::vector<short> tone(kFrames);
    for (int i = 0; i < kFrames; i++) tone[i] = static_cast<short>(12000 * std::sin(i * 2.0 * 3.14159265358979 * 440.0 / kRate));
    ALuint buffer = 0, source = 0;
    alGenBuffers(1, &buffer);
    alBufferData(buffer, AL_FORMAT_MONO16, tone.data(), kFrames * 2, kRate);
    alGenSources(1, &source);
    alSourcei(source, AL_BUFFER, static_cast<ALint>(buffer));
    Check(alGetError() == AL_NO_ERROR, "a buffer and a source");

    auto start = std::chrono::steady_clock::now();
    alSourcePlay(source);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    ALint offset = 0, state = 0;
    alGetSourcei(source, AL_SAMPLE_OFFSET, &offset);
    alGetSourcei(source, AL_SOURCE_STATE, &state);
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    double expected = elapsed * kRate;
    std::printf("      after %.3f s the source is at sample %d (the clock says %.0f)\n", elapsed, offset, expected);
    Check(state == AL_PLAYING, "the source is playing after half a second");
    // the backend keeps three updates of 20 ms queued: the mix runs that far ahead of the clock
    Check(offset > expected * 0.85 && offset < expected + kRate * 0.15, "the mix is consumed at the rate of the clock");

    std::this_thread::sleep_for(std::chrono::milliseconds(750));
    alGetSourcei(source, AL_SOURCE_STATE, &state);
    Check(state == AL_STOPPED, "the source stopped at the end of its second");

    alDeleteSources(1, &source);
    alDeleteBuffers(1, &buffer);
    alcMakeContextCurrent(nullptr);
    alcDestroyContext(context);
    Check(alcCloseDevice(device) == ALC_TRUE, "the device closes");
    return true;
}

int main() {
    PlayOnce(1);
    PlayOnce(2);  // closed and opened again
    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
