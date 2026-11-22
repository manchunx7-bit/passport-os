# SPDX-License-Identifier: Apache-2.0
"""Compile the entire low-memory chat implementation against a fake Link transport."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ChatLinkErrors(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        source = (ROOT / 'components/passport_muse/sdk/muse_chat_link.c').read_text()
        # Keep all production declarations/functions together. Only platform
        # includes are replaced; the public and shared-helper APIs stay real.
        source = source.replace('#include "muse_row_stream.inc"', (ROOT / 'components/passport_muse/sdk/muse_row_stream.inc').read_text())
        source = re.sub(r'^#include .*$', '', source, flags=re.MULTILINE)
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_compat.h"
#include "cJSON.h"
#include "muse_chat.h"
#include "muse_chat_priv.h"
#define ESP_LOGW(tag,...) ((void)(tag))
#define ESP_LOGI(tag,...) ((void)(tag))
#define portMAX_DELAY 0
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
typedef int SemaphoreHandle_t;
typedef int StaticSemaphore_t;
static int xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) { (void)s; return 1; }
static void xSemaphoreTake(int lock, int wait) { (void)lock; (void)wait; }
static void xSemaphoreGive(int lock) { (void)lock; }
static void vTaskDelay(int ticks) { (void)ticks; }
typedef struct { unsigned count, size; unsigned char data[8][80]; } fake_queue_t;
typedef fake_queue_t *QueueHandle_t;
static fake_queue_t queue;
typedef fake_queue_t StaticQueue_t;
static QueueHandle_t xQueueCreateStatic(unsigned count, unsigned size, uint8_t *items, StaticQueue_t *storage) {
    (void)items; (void)storage;
    assert(count == 8 && size <= 80); queue.size=size; return &queue;
}
static void xQueueReset(QueueHandle_t q) { q->count=0; }
static int xQueueSend(QueueHandle_t q, const void *p, int wait) {
    (void)wait; if(q->count==8) return 0;
    memcpy(q->data[q->count++],p,q->size); return 1;
}
static int xQueueReceive(QueueHandle_t q, void *p, int wait) {
    (void)wait; if(!q->count) return 0; memcpy(p,q->data[0],q->size);
    --q->count; memmove(q->data[0],q->data[1],q->count*sizeof(q->data[0])); return 1;
}
static int64_t now=10000000;
static int64_t esp_timer_get_time(void) { return now; }
static uint32_t esp_random(void) { return 42; }
static bool muse_link_hatch_linked(void) { return true; }
static bool muse_wifi_connected(void) { return true; }
static bool link_ready=true;
static bool muse_link_req_ready(void) { return link_ready; }
static const char *transport_error="QUEUE BUSY";
static const char *muse_link_req_error(void) { return transport_error; }
typedef void (*frame_fn)(void *,int,const uint8_t *,size_t,bool);
static struct { frame_fn cb; void *ctx; int id; } requests[2];
static int next_id, allocation_attempts, fail_allocation, fail_send, fail_at_n;
static size_t largest_reallocation;
static size_t max_allocation=SIZE_MAX;
static int operations[20], operation_count;
static void operation(int n) { if(operation_count<20) operations[operation_count++]=n; }
__attribute__((unused)) static void *test_malloc(size_t n) {
    allocation_attempts++;
    if(n>max_allocation) return NULL;
    if(fail_allocation) { fail_allocation--; return NULL; }
    return malloc(n);
}
__attribute__((unused)) static void *test_realloc(void *p,size_t n) {
    allocation_attempts++;
    if(n>largest_reallocation) largest_reallocation=n;
    if(n>max_allocation) return NULL;
    if(fail_allocation) { fail_allocation--; return NULL; }
    return realloc(p,n);
}
static void *test_calloc(size_t n,size_t size) {
    allocation_attempts++;
    if(n*size>max_allocation) return NULL;
    if (allocation_attempts==fail_at_n) return NULL;
    if(fail_allocation) { fail_allocation--; return NULL; }
    return calloc(n,size);
}
static int64_t muse_link_req_open(const char *verb,const char *path,const char *const *headers,
                                 bool end,frame_fn cb,void *ctx) {
    assert(!strcmp(verb,"POST") && !end);
    int slot=!strcmp(path,"/chat/subscribe");
    assert(slot || !strcmp(path,"/chat/stream"));
    if(slot) {
        bool ndjson=false;
        for(int i=0;headers[i];i+=2) if(!strcmp(headers[i],"Accept")) ndjson=!strcmp(headers[i+1],"application/x-ndjson");
        assert(ndjson);
    }
    requests[slot].cb=cb; requests[slot].ctx=ctx; requests[slot].id=++next_id;
    operation(slot ? 3 : 1); return next_id;
}
static bool muse_link_req_send(int64_t id,const void *data,size_t len,bool end,int wait) {
    (void)wait;
    if(fail_send) { fail_send--; return false; }
    if(id==requests[1].id) { assert(end && len==2 && !memcmp(data,"{}",2)); operation(4); }
    else {
        assert(id==requests[0].id);
        if(end) operation(5);
        else if(len>20 && ((const char*)data)[0]=='{') {
            assert(strstr(data,"\"output_modality\":\"text\"")); operation(2);
        }
    }
    return true;
}
static void muse_link_req_cancel(int64_t id) { (void)id; }
#define malloc test_malloc
#define realloc test_realloc
#define calloc test_calloc
''' + source + r'''
#undef malloc
#undef realloc
#undef calloc
static void feed(int slot,int status,const char *p,size_t n,bool end) {
    requests[slot].cb(requests[slot].ctx,status,(const uint8_t*)p,n,end);
}
static void sub(const char *p) { feed(RX_SUB,200,p,strlen(p),false); }
static void bytewise(const char *p) {
    for(size_t i=0;i<strlen(p);i++) feed(RX_SUB,200,p+i,1,false);
}
static void ack(const char *body) { feed(RX_NOTE,200,body,strlen(body),true); pump(); }
static void note_ack(void) { ack("{\"result\":{\"message_id\":\"note\",\"reply_to_message_id\":\"parent\"}}"); }
static void begin(void) {
    muse_hatch_turn_begin(); assert(s_turn.phase==T_TALKING);
    pump(); assert(s_turn.phase==T_TALKING);
}
static void release(void) { muse_hatch_turn_end(); assert(s_turn.phase==T_ACK); }
static void final(const char *id,const char *parent,const char *text) {
    char line[2048];
    snprintf(line,sizeof(line),"{\"type\":\"event\",\"event\":\"message.assistant\",\"payload\":{\"message_id\":\"%s\",\"reply_to_message_id\":\"%s\",\"display_text\":\"%s\"}}\n",id,parent,text);
    sub(line);
}
static void assert_replied(const char *text) {
    pump(); assert(s_turn.phase==T_REPLY && s_turn.replied); assert(!strcmp(s_turn.text,text));
}
static void streaming(void) {
    begin(); assert(operation_count==2 && operations[0]==1 && operations[1]==2);
    assert(!s_stream[RX_SUB]); release();
    assert(operation_count==5 && operations[2]==3 && operations[3]==4 && operations[4]==5);
    sub("{\"type\":\"event\",\"seq\":100,\"event\":\"agent.status\",\"payload\":{\"status\":\"idle\"}}\n");
    const char *events=
        "{\"type\":\"subscribed\"}\n"
        "{\"type\":\"event\",\"seq\":101,\"event\":\"delta.text_append\",\"payload\":{\"seq\":1,\"event_name\":\"message.assistant\",\"message_id\":\"reply\",\"parent_message_id\":\"note\",\"display_text\":null,\"content\":null,\"text\":\"Hello \"}}\n"
        "{\"type\":\"event\",\"seq\":102,\"event\":\"delta.text_append\",\"payload\":{\"seq\":1,\"event_name\":\"message.assistant\",\"type\":\"text\",\"message_id\":\"reply\",\"text\":\"world\"}}\n"
        "{\"type\":\"event\",\"seq\":103,\"event\":\"delta.message_done\",\"payload\":{\"seq\":1,\"event_name\":\"message.assistant\",\"message_id\":\"reply\"}}\n";
    bytewise(events); assert(s_pending_count==1 && !s_turn.replied);
    assert(!strcmp(s_pending[0].text,"Hello world"));
    final("reply","note","Hello world"); assert(s_pending_count==1);
    note_ack(); assert_replied("Hello world");
    final("reply","note","Hello world"); assert_replied("Hello world");
    char text[72]; assert(muse_hatch_turn_event(text,sizeof(text))==MUSE_HATCH_EV_SENT);
    assert(muse_hatch_turn_event(text,sizeof(text))==MUSE_HATCH_EV_REPLY);
    now+=20000000; pump(); assert(s_turn.phase==T_REPLY && s_stream[RX_SUB]);
}
static void correlation(void) {
    begin(); release();
    for(int i=0;i<5;i++) { char id[10]; snprintf(id,sizeof(id),"other%d",i); final(id,"elsewhere","Other chat"); }
    assert(s_pending_count==2); /* bounded provisional candidates, no fatal flag */
    final("reply","note","Early reply"); note_ack(); assert_replied("Early reply");
    for(int i=0;i<5;i++) { char id[10]; snprintf(id,sizeof(id),"other%d",i); final(id,"elsewhere","Other chat"); }
    assert(!s_pending_count); assert_replied("Early reply");
    final("second","parent","Second"); assert_replied("Second");
    final("unparented","","Live"); assert_replied("Live");
    muse_hatch_turn_cancel(); begin(); release();
    final("evicted","note","Too early"); final("other1","elsewhere","Other"); final("other2","elsewhere","Other");
    note_ack(); assert(!s_turn.replied && s_turn.phase==T_REPLY);
    now+=REPLY_TIMEOUT_US+1; pump();
    assert(s_turn.phase==T_IDLE && strstr(s_turn.error,"BUFFER LIMIT"));
    begin(); assert(!s_early_evicted); release(); note_ack();
    final("fresh","note","Recovered"); assert_replied("Recovered");
    muse_hatch_turn_cancel(); begin(); release(); note_ack();
    final("first","note","First"); final("second","note","Second"); final("third","note","Third");
    assert(s_pending_count==2 && !s_early_evicted);
    assert(!strcmp(s_pending[0].msg,"second") && !strcmp(s_pending[1].msg,"third"));
    assert_replied("Third"); /* newest two finals between voice-task polls */
}
static void oversized(void) {
    begin(); release(); note_ack();
    bytewise("{\"type\":\"event\",\"seq\":1,\"event\":\"delta.text_append\",\"payload\":{\"message_id\":\"reply\",\"parent_message_id\":\"note\",\"text\":\"Retained text\"}}\n");
    sub("{\"metadata\":{\"nested\":[{\"text\":\"");
    for(int i=0;i<5000;i++) sub("x");
    bytewise("\\\"escaped\\\\text\"}]},\"payload\":{\"message_id\":\"reply\"},\"type\":\"event\",\"seq\":2,\"event\":\"delta.message_done\"}\n");
    assert(largest_reallocation<=ROW_MAX); assert_replied("Retained text");
    muse_hatch_turn_cancel(); begin(); release(); note_ack();
    sub("{\"type\":\"event\",\"event\":\"message.assistant\",\"payload\":{\"message_id\":\"long\",\"display_text\":\"");
    for(int i=0;i<5000;i++) sub("x");
    sub("\",\"reply_to_message_id\":\"note\"}}\n");
    pump(); assert(s_turn.replied && strlen(s_turn.text)==TEXT_MAX-1);
    muse_hatch_turn_cancel(); begin(); release(); note_ack();
    for(unsigned i=0;i<ROW_STREAM_MAX+1;i++) sub("x");
    assert(s_skipped_big && s_rx[RX_SUB].overflow && largest_reallocation<=ROW_MAX);
    sub("\n{\"type\":\"subscribed\"}\r\n");
    assert(!s_rx[RX_SUB].body && !s_rx[RX_SUB].overflow);
    final("after","note","After oversized line"); assert_replied("After oversized line");
    muse_hatch_turn_cancel(); begin(); release(); note_ack();
    int before=allocation_attempts;
    fail_allocation=1; sub("{\"type\":"); sub("\"event\"}\n");
    assert(!s_receive_oom && !s_skipped_big && allocation_attempts==before);
    fail_allocation=0;
    final("no-alloc","note","No receive allocation"); assert_replied("No receive allocation");
    muse_hatch_turn_cancel(); begin(); release(); note_ack();
    bytewise("{\"type\":\"event\",\"event\":\"message.assistant\",\"payload\":{\"message_id\":\"last\",\"display_text\":\"Terminal line\"}}");
    feed(RX_SUB,200,NULL,0,true); assert_replied("Terminal line");
}
static void unicode_and_fields(void) {
    begin(); release(); note_ack();
    bytewise("{\"type\":\"event\",\"event\":\"message.assistant\",\"message_id\":\"envelope\",\"payload\":{\"type\":\"ignored\",\"message_id\":\"reply\",\"id\":\"fallback\",\"reply_to_message_id\":\"note\",\"parent_message_id\":\"elsewhere\",\"display_text\":\"Hi \\uD83D\\uDE00 café \\\"yes\\\"\\nnext\",\"content\":\"wrong\",\"text\":\"wrong\",\"metadata\":{\"display_text\":\"also wrong\"}},\"message_id\":\"late-envelope\"}\n");
    assert_replied("Hi 😀 café \"yes\"\nnext");
    assert(!strcmp(s_turn.seen[0],"reply"));
    muse_hatch_turn_cancel(); begin(); release(); note_ack();
    sub("{\"type\":\"event\",\"event\":\"delta.text_append\",\"payload\":{\"message_id\":\"long\",\"text\":\"");
    for(int i=0;i<TEXT_MAX-3;i++) sub("a");
    sub("\"}}\n");
    bytewise("{\"type\":\"event\",\"event\":\"delta.text_append\",\"payload\":{\"message_id\":\"long\",\"text\":\"😀tail\"}}\n");
    sub("{\"type\":\"event\",\"event\":\"delta.message_done\",\"payload\":{\"message_id\":\"long\"}}\n");
    pump(); assert(s_turn.replied && strlen(s_turn.text)==TEXT_MAX-3);
    muse_hatch_turn_cancel(); begin(); release(); note_ack();
    bytewise("{\"type\":\"event\",\"event\":\"message.assistant\",\"payload\":{\"message_id\":\"bad\",\"text\":\"\\uD800oops\"}}\n");
    assert(!s_pending_count); final("good","note","Recovered"); assert_replied("Recovered");
}
static void retry_and_stale(void) {
    /* A failed stage allocation must fail cleanly and allow the next turn. */
    fail_allocation=1; muse_hatch_turn_begin();
    assert(s_turn.phase==T_IDLE && strstr(s_turn.error,"OUT OF MEMORY"));
    fail_allocation=0; muse_hatch_turn_cancel();
    begin(); release();
    char big[ACK_MAX+1]; memset(big,'x',sizeof(big)); feed(RX_NOTE,200,big,sizeof(big),true);
    pump(); assert(s_turn.phase==T_IDLE && s_turn.error[0]);
    begin(); release();
    sub("{\"type\":\"event\",\"event\":\"message.assistant\",\"payload\":{\"text\":\"unfinished");
    frame_fn old=requests[RX_SUB].cb; void *old_ctx=requests[RX_SUB].ctx;
    muse_hatch_turn_cancel(); begin();
    old(old_ctx,403,NULL,0,true); pump(); assert(s_turn.phase==T_TALKING);
    fail_send=1; int16_t pcm[1024]={0}; muse_hatch_turn_audio(pcm,1024);
    assert(s_turn.phase==T_IDLE && strstr(s_turn.error,"KEEP UP"));
    begin(); release(); note_ack(); final("reply","note","Retry works"); assert_replied("Retry works");
}
static void denied_and_ack(void) {
    begin(); release(); note_ack();
    sub("partial"); sub("\n");
    now+=REPLY_TIMEOUT_US+1; pump();
    assert(s_turn.phase==T_IDLE && !strcmp(s_turn.error,"NO REPLY FROM MUSE"));
    begin(); release(); note_ack();
    for(unsigned i=0;i<ROW_STREAM_MAX+1;i++) sub("x");
    sub("\n");
    now+=REPLY_TIMEOUT_US+1; pump();
    assert(s_turn.phase==T_IDLE && !strcmp(s_turn.error,"REPLY EVENT TOO LARGE"));
    begin(); release(); note_ack();
    now+=REPLY_TIMEOUT_US+1; pump();
    assert(s_turn.phase==T_IDLE && !strcmp(s_turn.error,"NO REPLY FROM MUSE"));
    int statuses[]={403,401,404,-1}; const char *errors[]={"ACCESS DENIED (403)","AUTH REQUIRED (401)","HTTP 404","LOST CONNECTION"};
    for(unsigned i=0;i<4;i++) {
        begin(); release(); feed(RX_SUB,statuses[i],NULL,0,true); pump();
        assert(s_turn.phase==T_IDLE && strstr(s_turn.error,errors[i]));
    }
    begin(); release(); ack("{}"); assert(s_turn.phase==T_IDLE && s_turn.error[0]);
    begin(); release(); feed(RX_SUB,200,NULL,0,true); note_ack();
    assert(s_turn.phase==T_REPLY && s_turn.reconnecting);
    now+=SYNC_TIMEOUT_US+1; pump();
    assert(s_turn.phase==T_IDLE && strstr(s_turn.error,"SYNC INTERRUPTED"));
    begin(); release(); note_ack(); now+=REPLY_TIMEOUT_US+1; pump();
    assert(s_turn.phase==T_IDLE && strstr(s_turn.error,"NO REPLY"));
    begin(); release(); note_ack(); final("retry","note","Recovered"); assert_replied("Recovered");
}
static void transcript_and_lines(void) {
    begin(); release();
    int before=allocation_attempts;
    sub("{\"type\":\"event\",\"event\":\"message.user\",\"payload\":{\"message_id\":\"note\",\"display_text\":\"Hello\\n[file:audio/wav note]\"}}\r\n"
        "{\"type\":\"event\",\"event\":\"message.assistant\",\"payload\":{\"message_id\":\"reply\",\"reply_to_message_id\":\"note\",\"display_text\":\"Hi\"}}\n");
    assert(allocation_attempts==before); /* complete lines parse in Link's buffer */
    note_ack(); assert_replied("Hi");
    char text[72]; assert(muse_hatch_turn_event(text,sizeof(text))==MUSE_HATCH_EV_SENT);
    assert(muse_hatch_turn_event(text,sizeof(text))==MUSE_HATCH_EV_HEARD && !strcmp(text,"Hello"));
    assert(muse_hatch_turn_event(text,sizeof(text))==MUSE_HATCH_EV_REPLY);
}
static void delayed_result(void) {
    begin(); release(); note_ack(); final("first","note","Still working"); assert_replied("Still working");
    int64_t stream=s_stream[RX_SUB]; now+=180000000; pump();
    assert(s_turn.phase==T_REPLY && s_stream[RX_SUB]==stream);
    final("result","first","Here is the final result"); assert_replied("Here is the final result");
    char full[TEXT_MAX]; assert(muse_hatch_turn_reply(full,sizeof(full)) && !strcmp(full,"Here is the final result"));
    final("third","result","Follow-up"); assert_replied("Follow-up");
    final("third","result","Updated final"); assert_replied("Updated final");
    final("other","elsewhere","Unrelated"); assert_replied("Updated final");
    now+=3600000000LL; pump(); assert(s_stream[RX_SUB] && s_turn.phase==T_REPLY);
    frame_fn old=requests[RX_SUB].cb; void *ctx=requests[RX_SUB].ctx;
    muse_hatch_turn_cancel(); old(ctx,200,(const uint8_t*)"x",1,true);
    assert(s_turn.phase==T_IDLE && !s_pending_count && !s_stream[RX_SUB]);
}
static void recover_subscription(void) {
    begin(); release(); note_ack(); final("first","note","Still working"); assert_replied("Still working");
    frame_fn old=requests[RX_SUB].cb; void *ctx=requests[RX_SUB].ctx;
    feed(RX_SUB,-1,NULL,0,true); pump(); assert(s_turn.reconnecting && s_turn.phase==T_REPLY);
    now+=SYNC_RETRY_US; pump(); assert(s_stream[RX_SUB] && requests[RX_SUB].ctx!=ctx);
    old(ctx,403,NULL,0,true); assert(s_rx[RX_SUB].status==0);
    final("result","first","Result after reconnect"); assert_replied("Result after reconnect");
    assert(!s_turn.reconnecting);
    /* A new turn drops all correlation and receives no stale task result. */
    begin(); release(); note_ack(); final("old-child","first","Stale"); assert(!s_pending_count);
    final("fresh","note","Fresh"); assert_replied("Fresh");
    feed(RX_SUB,-1,NULL,0,true); pump(); link_ready=false; now+=SYNC_TIMEOUT_US+1; pump();
    assert(s_turn.phase==T_IDLE && strstr(s_turn.error,"SYNC INTERRUPTED"));
    assert(!strcmp(s_turn.text,"Fresh")); link_ready=true;
}
static void large_whole_rows(void) {
    begin(); release(); note_ack();
    char line[ROW_MAX+4096]; int n=snprintf(line,sizeof(line),"{\"type\":\"event\",\"event\":\"message.assistant\",\"payload\":{\"message_id\":\"large\",\"reply_to_message_id\":\"note\",\"display_text\":\"Result\"},\"metadata\":\"");
    memset(line+n,'x',ROW_MAX+100); n+=ROW_MAX+100; strcpy(line+n,"\"}\n");
    int before=allocation_attempts; sub(line); assert(allocation_attempts==before);
    assert_replied("Result");
    /* Terminal complete row also parses in place, with no newline. */
    muse_hatch_turn_cancel(); begin(); release(); note_ack();
    const char *last="{\"type\":\"event\",\"event\":\"message.assistant\",\"payload\":{\"message_id\":\"last\",\"reply_to_message_id\":\"note\",\"text\":\"Last row\"}}";
    before=allocation_attempts; feed(RX_SUB,200,last,strlen(last),true);
    assert(allocation_attempts==before); assert_replied("Last row");
}
static void text_modality(void) {
    cJSON *text=cJSON_Parse(MUSE_HATCH_NOTE_HEAD MUSE_HATCH_NOTE_TAIL);
    assert(text);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(text,"output_modality")),"text"));
    cJSON_Delete(text);
}
static void inactive_calls(void) {
    char text[72]="unchanged";
    int16_t pcm[4]={0};
    assert(!muse_hatch_initialized() && !muse_hatch_ready());
    int before=allocation_attempts;
    muse_hatch_turn_begin(); muse_hatch_turn_audio(pcm,4);
    muse_hatch_turn_end(); muse_hatch_turn_cancel();
    assert(!muse_hatch_turn_recording());
    assert(muse_hatch_turn_event(text,sizeof(text))==MUSE_HATCH_EV_NONE);
    assert(!muse_hatch_turn_reply(text,sizeof(text)));
    assert(!muse_hatch_turn_caption(0,text,sizeof(text)));
    assert(!strcmp(text,"unchanged") && allocation_attempts==before);
}
static void lifecycle(void) {
    inactive_calls();
    fail_allocation=1; muse_hatch_start(); inactive_calls();
    muse_hatch_stop(); inactive_calls();
    fail_at_n=allocation_attempts+2; muse_hatch_start();
    assert(s_turn_mem && !s_mem && !muse_hatch_initialized());
    muse_hatch_stop(); inactive_calls(); fail_at_n=0;
    muse_hatch_start(); assert(muse_hatch_ready());
    begin(); release(); note_ack(); final("reply","note","Before stop"); assert_replied("Before stop");
    muse_hatch_stop(); assert(s_events && !s_events->count); inactive_calls();
    muse_hatch_stop(); inactive_calls();
    muse_hatch_start(); assert(muse_hatch_ready());
    assert(muse_hatch_turn_event(NULL,0)==MUSE_HATCH_EV_NONE);
    begin(); release(); note_ack(); final("fresh","note","After restart"); assert_replied("After restart");
    muse_hatch_stop(); inactive_calls();
}
static void suspend_resume(void) {
    begin(); release(); note_ack(); final("first","note","Before reconnect"); assert_replied("Before reconnect");
    frame_fn stale=requests[RX_SUB].cb; void *old=requests[RX_SUB].ctx;
    link_ready=false;
    feed(RX_SUB,-1,NULL,0,true);
    muse_hatch_suspend();
    assert(!s_mem && s_turn_mem && !muse_hatch_ready());
    assert(!strcmp(s_turn.note_id,"note") && !strcmp(s_note_id,"note"));
    assert(s_turn.phase==T_REPLY && !s_rx[RX_SUB].body);
    stale(old,200,(const uint8_t*)"garbage",7,false);
    char reply[1024]; assert(muse_hatch_turn_reply(reply,sizeof(reply)) && !strcmp(reply,"Before reconnect"));
    fail_allocation=1; muse_hatch_start(); assert(!muse_hatch_ready() && !s_mem);
    muse_hatch_start(); link_ready=true; now+=SYNC_RETRY_US+1; pump();
    final("final","first","After reconnect"); assert_replied("After reconnect");
    stale(old,-1,NULL,0,true); assert_replied("After reconnect");
    muse_hatch_turn_cancel(); begin(); link_ready=false;
    muse_hatch_suspend(); assert(s_turn.phase==T_IDLE && !s_mem && !s_turn.stage && !s_turn.chunk);
    char text[72]; assert(muse_hatch_turn_event(text,sizeof(text))==MUSE_HATCH_EV_ERROR);
    muse_hatch_turn_cancel(); muse_hatch_stop(); inactive_calls();
    link_ready=true; muse_hatch_start(); begin(); release(); note_ack();
    link_ready=false; feed(RX_SUB,-1,NULL,0,true); muse_hatch_suspend();
    while(muse_hatch_turn_event(text,sizeof(text))!=MUSE_HATCH_EV_NONE) {}
    now+=SYNC_TIMEOUT_US+1;
    assert(muse_hatch_turn_event(text,sizeof(text))==MUSE_HATCH_EV_ERROR && strstr(text,"SYNC INTERRUPTED"));
    muse_hatch_stop(); inactive_calls();
}
static void classified_errors(void) {
    link_ready=false; int before=allocation_attempts, request_count=next_id;
    muse_hatch_turn_begin(); assert(s_turn.phase==T_IDLE && strstr(s_turn.error,"NOT READY"));
    assert(allocation_attempts==before && request_count==next_id); link_ready=true;
    const int statuses[]={401,403,429,502,-3,-4};
    const char *reasons[]={"AUTH","DENIED","REQUEST ERROR","SERVER ERROR","QUEUE BUSY","PROTOCOL ERROR"};
    for(unsigned i=0;i<6;i++){
        begin(); release(); feed(RX_NOTE,statuses[i],NULL,0,true); pump();
        assert(s_turn.phase==T_IDLE && strstr(s_turn.error,reasons[i]));
    }
    begin(); release(); ack("{\"result\":{}}"); assert(strstr(s_turn.error,"INVALID RESPONSE"));
    begin(); release(); ack("not json"); assert(strstr(s_turn.error,"INVALID RESPONSE"));
    transport_error="OUT OF MEMORY"; fail_send=1; muse_hatch_turn_begin();
    assert(strstr(s_turn.error,"OUT OF MEMORY"));
    transport_error="QUEUE BUSY"; begin(); release(); note_ack();
    int attempts=allocation_attempts;
    bytewise("{\"type\":\"subscribed\"}\n"); assert(allocation_attempts==attempts);
}
static void fragmented_heap(void) {
    muse_hatch_stop();
    max_allocation=3000; /* less than both the old 8248-byte parser and large rows */
    muse_hatch_start(); assert(muse_hatch_ready());
    begin(); release(); note_ack();
    sub("{\"type\":\"event\",\"event\":\"message.assistant\",\"metadata\":\"");
    for(int i=0;i<100000;i++) sub("x");
    bytewise("\",\"payload\":{\"message_id\":\"fragmented\",\"reply_to_message_id\":\"note\",\"display_text\":\"Ready \\u4e2d\\u6587 \\ud83d\\ude00\"}}\n");
    assert_replied("Ready 中文 😀");
    assert(!s_receive_oom && !largest_reallocation);
    assert(!s_rx[RX_SUB].len);
    for(unsigned split=1;split<=12;split++) {
        muse_hatch_turn_cancel(); begin(); release(); note_ack();
        const char *head="{\"metadata\":\"";
        const char *tail="\",\"type\":\"event\",\"event\":\"message.assistant\",\"payload\":{\"message_id\":\"page\",\"display_text\":\"";
        size_t padding=1024-split-strlen(head)-strlen(tail);
        sub(head); for(size_t j=0;j<padding;j++) sub("x"); sub(tail);
        bytewise("\\u4e2d\\ud83d\\ude00\"}}\n");
        assert_replied("中😀");
    }
    muse_hatch_turn_cancel(); begin(); release(); note_ack();
    sub("{\"metadata\":\"");
    for(int i=0;i<1500;i++) sub("x");
    muse_hatch_suspend();
    assert(!s_rx[RX_SUB].len);
    muse_hatch_stop(); assert(!s_mem && !s_turn_mem);
    for(int i=1;i<=8;i++) {
        fail_at_n=allocation_attempts+i; muse_hatch_start();
        assert(!muse_hatch_initialized());
        muse_hatch_stop(); assert(!s_mem && !s_turn_mem);
    }
    fail_at_n=0; muse_hatch_start(); assert(muse_hatch_ready());
}
static void very_large_streamed_rows(void) {
    const size_t splits[]={1,7,1023,4096,16384};
    char *line=malloc(1500000); assert(line);
    for(unsigned attempt=0;attempt<sizeof(splits)/sizeof(splits[0]);attempt++) {
        muse_hatch_turn_cancel();begin();release();note_ack();
        size_t n=(size_t)sprintf(line,"{\"metadata\":[{\"display_text\":\"wrong\",\"audio\":\"");
        memset(line+n,'a',1300000);n+=1300000;
        n+=(size_t)sprintf(line+n,"\"}],\"payload\":{\"content\":\"fallback\",\"display_text\":\"Ready \\u4e2d\\u6587 \\ud83d\\ude00\",\"text\":null,\"message_id\":\"large-%u\",\"reply_to_message_id\":\"note\"},\"message_id\":\"late-envelope\",\"type\":\"event\",\"seq\":18446744073709551615,\"event\":\"message.assistant\"}\n",attempt);
        int before=allocation_attempts;
        for(size_t pos=0;pos<n;) {size_t take=splits[attempt];if(take>n-pos)take=n-pos;feed(RX_SUB,200,line+pos,take,false);pos+=take;}
        assert(allocation_attempts==before && !s_skipped_big && !s_receive_oom);
        assert_replied("Ready 中文 😀");
        assert(s_last_seq==UINT64_MAX);
    }
    muse_hatch_turn_cancel();begin();release();note_ack();
    sub("{\"type\":\"event\",\"event\":\"message.assistant\",\"payload\":{\"content\":\"");
    memset(line,'a',1300000);feed(RX_SUB,200,line,1300000,false);
    bytewise("\",\"display_text\":\"Short preferred caption\",\"message_id\":\"preferred\",\"reply_to_message_id\":\"note\"}}\n");
    assert_replied("Short preferred caption");
    muse_hatch_turn_cancel();begin();release();note_ack();
    const char *bad[]={
        "{\"type\":\"event\",\"event\":\"message.assistant\",\"payload\":{\"message_id\":\"bad\",\"text\":\"must not appear\",}}\n",
        "{\"type\":\"event\",\"seq\":18446744073709551616,\"event\":\"message.assistant\",\"payload\":{\"message_id\":\"bad\",\"text\":\"must not appear\"}}\n",
        "{\"type\":\"event\",\"event\":\"message.assistant\",\"payload\":{\"message_id\":\"bad\",\"text\":\"bad \\uD83D x\"}}\n"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++){bytewise(bad[i]);assert(!s_pending_count);}
    final("valid","note","After malformed rows");assert_replied("After malformed rows");
    free(line);
}
int main(int argc,char **argv) {
    assert(argc==2);
    if(atoi(argv[1])==11) { lifecycle(); return 0; }
    muse_hatch_start();
    switch(atoi(argv[1])) {
    case 0: streaming(); break;
    case 1: correlation(); break;
    case 2: oversized(); break;
    case 3: unicode_and_fields(); break;
    case 4: retry_and_stale(); break;
    case 5: denied_and_ack(); break;
    case 6: text_modality(); break;
    case 7: transcript_and_lines(); break;
    case 8: delayed_result(); break;
    case 9: recover_subscription(); break;
    case 10: large_whole_rows(); break;
    case 12: suspend_resume(); break;
    case 13: classified_errors(); break;
    case 14: fragmented_heap(); break;
    case 15: very_large_streamed_rows(); break;
    default: return 2;
    }
    muse_hatch_turn_cancel();
    return 0;
}
'''
        c = Path(cls.tmp.name) / 'chat.c'
        c.write_text(code, encoding='utf-8')
        cls.binary = c.with_suffix('')
        cjson = Path(os.environ.get('IDF_PATH', '/opt/esp-idf')) / 'components/json/cJSON'
        compiled = subprocess.run([
            os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
            '-I', str(cjson), '-I', str(ROOT / 'tests/muse_mocks'), '-I', str(ROOT / 'components/passport_muse/sdk'),
            '-fsanitize=address,undefined', str(c), str(ROOT / 'components/passport_muse/voice_helpers.c'), str(cjson / 'cJSON.c'), '-o', str(cls.binary)
        ], capture_output=True, text=True)
        if compiled.returncode:
            raise RuntimeError(compiled.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, number):
        subprocess.run([str(self.binary), str(number)], check=True)

    def test_delayed_final_after_minutes_and_assistant_parent_chain(self):
        self.run_case(8)

    def test_suspend_releases_parsers_preserves_reply_and_rejects_stale_frames(self):
        self.run_case(12)

    def test_first_send_not_ready_http_causes_and_fragment_allocations(self):
        self.run_case(13)

    def test_fragmented_heap_unicode_boundaries_and_each_parser_allocation_failure(self):
        self.run_case(14)

    def test_megabyte_rows_fragmentation_unicode_precedence_without_allocation(self):
        self.run_case(15)

    def test_reconnect_preserves_ids_and_cancel_drops_stale_frames(self):
        self.run_case(9)

    def test_large_and_terminal_whole_rows_do_not_allocate(self):
        self.run_case(10)

    def test_lazy_start_failure_stop_and_restart_are_safe(self):
        self.run_case(11)

    def test_fragmented_early_reply_ack_order_and_deduplication(self):
        self.run_case(0)

    def test_unrelated_events_do_not_fill_reply_queue(self):
        self.run_case(1)

    def test_large_final_metadata_and_caption_use_bounded_receive_memory(self):
        self.run_case(2)

    def test_field_precedence_escaping_and_utf8_boundaries(self):
        self.run_case(3)

    def test_retry_after_oom_overflow_send_failure_and_cancelled_frames(self):
        self.run_case(4)

    def test_authorization_failures_closed_stream_and_timeout_recover(self):
        self.run_case(5)

    def test_shared_voice_note_format_requests_text_replies(self):
        self.run_case(6)

    def test_transcript_and_multiple_complete_lines_need_no_receive_allocation(self):
        self.run_case(7)

if __name__ == "__main__":
    unittest.main()
