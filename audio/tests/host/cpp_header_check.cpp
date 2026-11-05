// Compile-only check: the public headers work from C++ and support an RAII wrapper over a context.

#include <utility>

#include "audio.h"
#include "audio_core.h"

namespace
{
class Context
{
public:
    Context()
    {
        status_ = audio_context_create(nullptr, &context_);
    }

    ~Context()
    {
        audio_context_destroy(context_);
    }

    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;

    Context(Context &&other) noexcept : context_(std::exchange(other.context_, nullptr)), status_(other.status_) {}

    int status() const { return status_; }

    int playSound(audio_sound_t sound) { return audio_ctx_play_sound(context_, sound); }

    int setVolume(int channel, int percent) { return audio_ctx_channel_set_volume(context_, channel, percent); }

    int startStream(int rateHz, int bits, int channels)
    {
        audio_stream_format_t format{rateHz, bits, channels};
        return audio_ctx_stream_start(context_, &format);
    }

private:
    audio_context_t *context_ = nullptr;
    int status_ = 0;
};

[[maybe_unused]] void use()
{
    Context context;
    Context moved = std::move(context);

    if (moved.status() == AUDIO_OK) {
        moved.startStream(44100, 16, 2);
        moved.setVolume(0, 80);
        moved.playSound(AUDIO_SOUND_INVALID);
    }
}
} // namespace

int main()
{
    return 0;
}
