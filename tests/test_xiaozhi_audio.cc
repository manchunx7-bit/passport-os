#include "mock_platform.h"
#define private public
#include "apps/xiaozhi/xiaozhi_audio.h"
#undef private
#include "../main/apps/xiaozhi/opus_encoder_storage.cc"
#define malloc mock_malloc
#define free mock_free
#include "../main/apps/xiaozhi/xiaozhi_audio.cc"
#undef malloc
#undef free

static size_t live_bytes() {size_t n=0;for(auto [p,size]:mock_allocations){(void)p;n+=size;}return n;}
static void clean(xiaozhi::AudioService &a) {
    a.work_=xiaozhi::AudioService::Work::Idle;
    xSemaphoreGive(a.worker_stopped_);assert(a.Deinit());assert(mock_allocations.empty());
    assert(audio_workspace::owner==audio_workspace::Owner::None);
}
int main() {
    // The UI snapshot follows PCM, expires on a stalled source, and survives tick
    // wrap. Publishing/reading it must not construct or allocate an audio service.
    int16_t meter_pcm[960];for(auto &sample:meter_pcm)sample=4096;
    xiaozhi::publish_level(reinterpret_cast<uint8_t *>(meter_pcm),sizeof(meter_pcm));
    assert(xiaozhi::AudioService::VisualLevel()>0&&mock_allocations.empty());
    mock_time+=181;assert(xiaozhi::AudioService::VisualLevel()==0);
    mock_time=0xFFFFF0;
    xiaozhi::publish_level(reinterpret_cast<uint8_t *>(meter_pcm),sizeof(meter_pcm));
    mock_time+=50;assert(xiaozhi::AudioService::VisualLevel()>0);
    mock_time+=181;assert(xiaozhi::AudioService::VisualLevel()==0);
    mock_time=1000;
    auto &a=xiaozhi::AudioService::GetInstance();
    assert(audio_workspace::acquire(audio_workspace::Owner::Radio));
    assert(a.Init()==ESP_ERR_NO_MEM);assert(mock_allocations.empty());
    assert(!audio_workspace::release(audio_workspace::Owner::Xiaozhi));
    assert(audio_workspace::release(audio_workspace::Owner::Radio));
    mock_fail_size=2880;assert(a.Init()==ESP_ERR_NO_MEM);assert(mock_allocations.empty());
    mock_fail_size=-1;mock_largest=8192;
    mock_encoder_bytes=audio_workspace::kAuxBytes+1;
    assert(a.Init()==ESP_ERR_NO_MEM);assert(mock_allocations.empty());
    mock_encoder_bytes=25000;mock_encoder_fail=true;
    assert(a.Init()==ESP_ERR_NO_MEM);assert(mock_allocations.empty());
    mock_encoder_fail=false;
    mock_largest=100000;mock_decoder_fail=true;
    assert(a.Init()==ESP_ERR_NO_MEM);assert(mock_allocations.empty());
    mock_decoder_fail=false;mock_decoder_bytes=audio_workspace::kAuxBytes+1;
    assert(a.Init()==ESP_ERR_NO_MEM);assert(mock_allocations.empty());
    mock_decoder_bytes=17784;mock_task_fail=true;
    assert(a.Init()==ESP_ERR_NO_MEM);assert(mock_allocations.empty());
    assert(audio_workspace::owner==audio_workspace::Owner::None);
    mock_task_fail=false;mock_alloc_order.clear();
    std::memset(audio_workspace::storage,0x5a,audio_workspace::kBytes);
    assert(a.Init()==ESP_OK);
    for(size_t i=0;i<audio_workspace::kRealtimeBytes;i++) assert(audio_workspace::storage[i]==0x5a);
    for(size_t i=mock_encoder_bytes;i<audio_workspace::kAuxBytes;i++) assert(audio_workspace::aux()[i]==0x5a);
    assert(mock_alloc_order.size()==3);
    assert(mock_alloc_order[0]==56);
    assert(mock_alloc_order[1]==2880 && mock_alloc_order[2]==512);
    assert(a.opus_dec_handle_==audio_workspace::aux());
    const size_t boot_bytes=live_bytes();
    // Handshake fragmentation must not require reallocating the existing codec.
    mock_largest=14848;assert(a.PrepareSessionResources());assert(a.ReleaseSessionResources());
    assert(live_bytes()==boot_bytes);
    a.SetPlaybackSampleRate(16000);assert(a.PrepareSessionResources());assert(a.ReleaseSessionResources());
    mock_largest=17408;assert(a.PrepareSessionResources());assert(a.ReleaseSessionResources());
    a.SetPlaybackSampleRate(24000);assert(a.PrepareSessionResources());assert(a.ReleaseSessionResources());
    assert(live_bytes()==boot_bytes);mock_largest=100000;
    mock_fail_size=3072;
    assert(!a.PrepareSessionResources());assert(a.ReleaseSessionResources());
    assert(live_bytes()==boot_bytes);mock_fail_size=-1;
    assert(a.PrepareSessionResources());const size_t session_bytes=live_bytes();
    assert(session_bytes==6976); // codec state and worker stack use fixed storage
    mock_format_fail=ESP_FAIL;assert(!a.StartRecording());mock_format_fail=0;
    // Ordinary turns reuse every allocation. Pending worker work blocks format
    // changes and frees, avoiding buffer sharing while an old worker is alive.
    const size_t count=mock_alloc_order.size();mock_largest=8192;assert(a.StartRecording());
    assert(a.opus_dec_handle_==nullptr&&a.opus_enc_handle_!=nullptr);
    assert(!a.OpenDecoder());
    // Feed the real recording section, stop on its second read, and inspect the
    // preceding captured frame. No extra buffers or tasks may be allocated.
    auto *capture=reinterpret_cast<int16_t *>(a.pcm_scratch_);
    for(size_t i=0;i<xiaozhi::kEncPcmFrameBytes/2;++i)capture[i]=4096;
    int reads=0;uint8_t captured_level=0;
    mock_on_read=[&] {if(++reads==2){captured_level=a.VisualLevel();a.is_recording_=false;}};
    a.RecordSection();assert(captured_level>0);mock_on_read={};
    a.is_recording_=true;
    assert(!a.StartPlayback());assert(!a.ReleaseSessionResources());
    assert(live_bytes()==session_bytes+56);
    a.work_=xiaozhi::AudioService::Work::Idle;a.is_recording_=false;
    assert(a.StartPlayback());assert(mock_alloc_order.size()==count+1);
    assert(a.opus_enc_handle_==nullptr&&a.opus_dec_handle_==audio_workspace::aux());
    assert(!a.EnsureEncoder());assert(live_bytes()==session_bytes);
    const uint8_t frame[64]={0};const uint32_t prior=a.PlaybackProgress();
    a.PushIncomingAudio(frame,sizeof(frame));assert(a.PlaybackProgress()!=prior);
    a.play_drain_=true;mock_on_write=[&a] {a.play_stop_=true;};
    const uint32_t queued=a.PlaybackProgress();a.PlaySection();
    assert(a.PlaybackProgress()!=queued);mock_on_write={};
    // A successfully written decoded frame publishes its amplitude; mute and
    // abort cannot keep a phantom waveform running.
    a.play_stop_=false;a.PushIncomingAudio(frame,sizeof(frame));
    for(size_t i=0;i<xiaozhi::kMaxPcmFrameBytes/2;++i)capture[i]=4096;
    a.PlaySection();assert(a.VisualLevel()>0);
    mock_master_volume=0;a.PushIncomingAudio(frame,sizeof(frame));
    a.PlaySection();assert(a.VisualLevel()==0);mock_master_volume=100;
    // More than two seconds of draining progress must not be hard-stopped.
    a.work_=xiaozhi::AudioService::Work::Play;a.is_playing_=true;a.play_stop_=false;
    int waits=0;mock_on_wait=[&] {
        a.playback_progress_.fetch_add(1);
        if(++waits==60) a.work_=xiaozhi::AudioService::Work::Idle;
    };
    const int64_t started=mock_time;a.StopPlayback();
    assert(mock_time-started>=3000 && !a.play_stop_);
    // An app exit must cancel a progressing drain instead of blocking the
    // controller until every buffered frame finishes.
    a.work_=xiaozhi::AudioService::Work::Play;a.is_playing_=true;a.play_stop_=false;
    waits=0;mock_on_wait=[&] {
        if(++waits==1) xiaozhi::AudioService::RequestStop();
        else a.work_=xiaozhi::AudioService::Work::Idle;
    };
    const int64_t exit_started=mock_time;a.StopPlayback();
    assert(mock_time-exit_started<=850 && a.play_stop_);
    assert(!a.StartRecording());
    mock_on_wait={};a.work_=xiaozhi::AudioService::Work::Idle;
    assert(!a.Deinit());assert(audio_workspace::owner==audio_workspace::Owner::Xiaozhi);
    assert(!audio_workspace::acquire(audio_workspace::Owner::Radio));
    clean(a);
    assert(a.Init()==ESP_OK);
    for(int i=0;i<100;++i) {
        assert(a.EnsureEncoder());assert(a.opus_dec_handle_==nullptr);
        a.work_=xiaozhi::AudioService::Work::Record;
        assert(!a.OpenDecoder());
        a.work_=xiaozhi::AudioService::Work::Idle;
        assert(a.OpenDecoder());assert(a.opus_enc_handle_==nullptr);
    }
    for(size_t i=0;i<audio_workspace::kRealtimeBytes;i++)assert(audio_workspace::storage[i]==0x5a);
    for(size_t i=mock_encoder_bytes;i<audio_workspace::kAuxBytes;i++)assert(audio_workspace::aux()[i]==0x5a);
    clean(a);
    for(int i=0;i<5;++i) {assert(a.Init()==ESP_OK);assert(a.PrepareSessionResources());clean(a);}
    int error=99;
    auto *fallback=__wrap_opus_encoder_create(16000,1,2049,&error);
    assert(fallback&&error==0&&!mock_allocations.empty());
    __wrap_opus_encoder_destroy(fallback);assert(mock_allocations.empty());
    std::puts("Xiaozhi audio: shared buffers, large allocation order, failure cleanup, reuse/drain PASS");
}
