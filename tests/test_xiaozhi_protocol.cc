#define XIAOZHI_REAL_JSON
#include "mock_platform.h"
#define private public
#include "apps/xiaozhi/xiaozhi_protocol.h"
#include "apps/xiaozhi/xiaozhi_audio.h"
#undef private
#include "../main/apps/xiaozhi/xiaozhi_protocol.cc"
const char *const kNsXiaozhi="xiaozhi";
std::string store_get_string(const char *,const char *,const char *) {return "client-id";}
bool store_set_string(const char *,const char *,const std::string &) {return true;}
static int accepted_audio=0;
static int tool_executions=0;
namespace device_control {void cancel_session(){} void response_sent(uint64_t,bool){} }
namespace reminders {
std::string mcp_result(const char *,const char *,bool *error,uint64_t) {
    ++tool_executions; *error=false; return "{\"saved\":true}";
}
}
namespace xiaozhi {
AudioService::AudioService() {} AudioService::~AudioService() {}
AudioService &AudioService::GetInstance() {static AudioService a;return a;}
void AudioService::PushIncomingAudio(const uint8_t *,size_t) {++accepted_audio;}
}
int main() {
    using namespace xiaozhi;Protocol p;auto q=xQueueCreate(1,sizeof(Event));p.SetEventQueue(q);
    const auto original_mac=p.mac_address_, original_client=p.client_id_;
    p.Publish(EvType::AiSentence,"fills the queue");assert(q->entries.size()==1);
    p.Publish(EvType::Connected,nullptr);p.Publish(EvType::HelloOk,nullptr);
    p.Publish(EvType::Disconnected,nullptr);p.Publish(EvType::SocketError,nullptr);
    Event event;const EvType order[]={EvType::Disconnected,EvType::SocketError,EvType::Connected,EvType::HelloOk};
    for(auto expected:order) {assert(p.PopCriticalEvent(&event));assert(event.type==expected);}
    assert(!p.PopCriticalEvent(&event));assert(q->entries.size()==1);
    p.SetState(State::Thinking);p.SetState(State::Speaking);assert(p.GetState()==State::Speaking);
    mock_fail_size=kMaxTextMessage;
    assert(!p.Start("wss://example.invalid/","",3)&&!p.rx_text_);
    mock_fail_size=-1;
    assert(p.Start("wss://example.invalid/","",3)&&p.rx_text_);
    p.client_=reinterpret_cast<void *>(1);p.hello_ok_=false;
    assert(!p.TriggerStartListen());assert(mock_sent_text.empty());
    p.hello_ok_=true;assert(!p.TriggerStartListen()); // missing session id
    p.session_id_="session";assert(p.TriggerStartListen());assert(mock_sent_text.size()==1);
    const char binary[]={0,0,0,1,1};
    esp_websocket_event_data_t data{binary,5,0,5,2,{0}};
    p.SetState(State::Idle);p.HandleData(&data);assert(accepted_audio==0);
    p.SetState(State::Speaking);p.HandleData(&data);assert(accepted_audio==1);
    p.hello_ok_=false;p.HandleData(&data);assert(accepted_audio==1);
    // Real JSON parser + production callback: a goodbye invalidates the
    // session even if the socket remains open and the text queue is full.
    p.hello_ok_=true;
    const char goodbye[]="{\"type\":\"goodbye\"}";
    esp_websocket_event_data_t bye{goodbye,sizeof(goodbye)-1,0,sizeof(goodbye)-1,1,{0}};
    const unsigned stops=mock_ws_stops, destroys=mock_ws_destroys;
    p.HandleData(&bye);
    assert(p.NeedsRestart() && !p.HandshakeDone() && p.GetState()==State::Disconnected);
    assert(!p.TriggerStartListen());
    assert(p.PopCriticalEvent(&event) && event.type==EvType::Disconnected);
    assert(q->entries.size()==1); // lifecycle event bypasses full display queue
    p.WebsocketEventHandler(&p,nullptr,WEBSOCKET_EVENT_CLOSED,nullptr);
    p.WebsocketEventHandler(&p,nullptr,WEBSOCKET_EVENT_FINISH,nullptr);
    assert(!p.PopCriticalEvent(&event)); // one terminal notification
    assert(mock_ws_stops==stops && mock_ws_destroys==destroys); // never stop in callback
    const char late[]="{\"type\":\"hello\",\"session_id\":\"late\"}";
    esp_websocket_event_data_t late_hello{late,sizeof(late)-1,0,sizeof(late)-1,1,{0}};
    p.HandleData(&late_hello);p.HandleData(&data);
    assert(!p.HandshakeDone() && accepted_audio==1);
    // Clean CLOSE without goodbye and unexpected task FINISH both retire the
    // handle. A transient disconnect still permits SDK auto-reconnection.
    for (int terminal : {WEBSOCKET_EVENT_CLOSED,WEBSOCKET_EVENT_FINISH}) {
        assert(p.Start("wss://example.invalid/","",3));p.hello_ok_=true;
        p.WebsocketEventHandler(&p,nullptr,terminal,nullptr);
        assert(p.NeedsRestart() && !p.HandshakeDone());
        assert(p.PopCriticalEvent(&event) && event.type==EvType::Disconnected);
    }
    assert(p.Start("wss://example.invalid/","",3));
    p.WebsocketEventHandler(&p,nullptr,WEBSOCKET_EVENT_DISCONNECTED,nullptr);
    assert(!p.NeedsRestart());
    p.Stop();assert(!p.PopCriticalEvent(&event));
    assert(p.Start("wss://example.invalid/","",3));
    p.HandleData(&late_hello);
    assert(p.HandshakeDone() && p.session_id()=="late");
    // Receive callbacks only enqueue; the controller performs the operation.
    const char call[]=R"({"type":"mcp","payload":{"id":42,"method":"tools/call","params":{"name":"self.reminder.create","arguments":{"title":"喝水","delay_seconds":600}}}})";
    esp_websocket_event_data_t rpc{call,sizeof(call)-1,0,sizeof(call)-1,1,{0}};
    p.HandleData(&rpc); assert(tool_executions==0);
    assert(p.ProcessMcp() && tool_executions==1);
    const auto saved_reply=mock_sent_text.back();
    p.HandleData(&rpc); assert(p.ProcessMcp() && tool_executions==1);
    assert(mock_sent_text.back()==saved_reply);
    // An interleaved call cannot make a retry execute a relative adjustment twice.
    const char status[]=R"({"type":"mcp","payload":{"id":43,"method":"tools/call","params":{"name":"self.device.get_status","arguments":{}}}})";
    esp_websocket_event_data_t status_rpc{status,sizeof(status)-1,0,sizeof(status)-1,1,{0}};
    p.HandleData(&status_rpc);assert(p.ProcessMcp()&&tool_executions==2);
    p.HandleData(&rpc);assert(p.ProcessMcp()&&tool_executions==2);
    assert(mock_sent_text.back()==saved_reply);
    assert(!p.ProcessMcp());
    // A complete valid JSON prefix followed by oversized whitespace is rejected.
    std::string oversized=std::string(call)+std::string(2100,' ');
    esp_websocket_event_data_t huge{oversized.data(),int(oversized.size()),0,int(oversized.size()),1,{0}};
    p.HandleData(&huge); assert(!p.ProcessMcp() && tool_executions==2);
    // Every refused request with a usable id is answered, even in a burst.
    const auto send_rpc=[&](const std::string &text){
        esp_websocket_event_data_t frame{text.data(),int(text.size()),0,int(text.size()),1,{0}};p.HandleData(&frame);
    };
    const auto rpc_with=[](int id,const char *method){
        return "{\"type\":\"mcp\",\"payload\":{\"id\":"+std::to_string(id)+",\"method\":\""+method+"\",\"params\":{}}}";
    };
    for(int id=60;id<65;++id) send_rpc(rpc_with(id,"tools/list"));
    send_rpc(rpc_with(70,"tools/a_method_name_longer_than_limit"));
    std::string refused;
    for(int i=0;i<6;++i){assert(p.ProcessMcp());refused+=mock_sent_text.back();}
    for(int id:{63,64,70}) assert(refused.find("\"id\":"+std::to_string(id)+",\"error\"")!=std::string::npos);
    assert(tool_executions==5&&!p.ProcessMcp());
    delete q;p.SetEventQueue(nullptr);
    p.Stop();assert(p.mac_address_.empty()&&p.client_id_.empty());
    assert(p.Start("wss://example.invalid/", "", 3));
    assert(p.mac_address_==original_mac&&p.client_id_==original_client);
    p.Stop();assert(p.mac_address_.empty()&&p.client_id_.empty());
    assert(!p.rx_text_&&mock_allocations.empty());
    // PING wakes a connected worker before stop/join. A failed PING must not
    // prevent cleanup, and a disconnected client must not send anything.
    for (bool connected : {true,false}) for (int result : {0,-1}) {
        mock_ws_connected=true;
        assert(p.Start("wss://example.invalid/","",3));
        mock_ws_connected=connected;mock_ws_ping_result=result;
        mock_ws_lifecycle.clear();mock_ws_ping_timeout=0;
        const unsigned pings=mock_ws_pings;
        p.Stop();
        const std::vector<std::string> expected=connected ?
            std::vector<std::string>{"ping","stop","destroy"} :
            std::vector<std::string>{"stop","destroy"};
        assert(mock_ws_lifecycle==expected);
        assert(mock_ws_pings==pings+(connected?1:0));
        assert(mock_ws_ping_timeout==(connected?pdMS_TO_TICKS(100):0));
        assert(!p.client_&&!p.rx_text_&&mock_allocations.empty());
        assert(!p.PopCriticalEvent(&event));
        p.Stop();assert(mock_ws_lifecycle==expected); // repeated stop is idle
    }
    mock_ws_connected=true;mock_ws_ping_result=0;
    std::puts("Xiaozhi protocol: goodbye/clean close/finish recovery, critical mailbox, stale-audio guards PASS");
}
