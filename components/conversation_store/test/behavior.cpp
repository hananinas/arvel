#include "conversation_store.hpp"
#include "cJSON.h"
#include <cassert>
#include <atomic>
#include <filesystem>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>
#include "sqlite3.h"

using namespace conversation_store;
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
Json packet(const Result &r){if(!r.ok()){fprintf(stderr,"storage error=%d sqlite=%d\n",int(r.error),r.sqlite_code);}assert(r.ok());Json j(cJSON_Parse(r.json.c_str()),cJSON_Delete);assert(j);assert(r.json.size()<=kOutputLimit);return j;}
void require_ok(const Result &r,const char *operation){if(!r.ok())fprintf(stderr,"%s: error=%d sqlite=%d details=%s\n",operation,int(r.error),r.sqlite_code,r.json.c_str());assert(r.ok());}
void ready(const Config &c){assert(start_async(c)==Error::none);for(unsigned n=0;n<10000&&status().state==State::mounting;++n)std::this_thread::sleep_for(std::chrono::milliseconds(1));if(status().state!=State::ready)fprintf(stderr,"initialize state=%d sqlite=%d\n",int(status().state),status().sqlite_code);assert(status().state==State::ready);}
std::string create(){auto j=packet(new_conversation("host behavioral archive"));return cJSON_GetObjectItemCaseSensitive(j.get(),"uuid")->valuestring;}
int rows(const Result &r){auto j=packet(r);auto *messages=cJSON_GetObjectItemCaseSensitive(j.get(),"messages");assert(cJSON_IsArray(messages));return cJSON_GetArraySize(messages);}
ToolIdentity id_for(const std::string &uuid,const char *call,const char *args="{\"brightness\":42,\"on\":true}"){ToolIdentity id;id.conversation=uuid.c_str();id.provider_session="provider-session";id.call_id=call;id.name="niko_control";id.arguments_json=args;id.run_id="host-run";id.generation=7;return id;}
bool cancel(void *){return true;}
struct Hold { std::atomic<bool> entered{false}, release{false}; };
bool hold_worker(void *context){auto *h=static_cast<Hold *>(context);h->entered=true;while(!h->release.load())std::this_thread::yield();return false;}
void bounded_queue(const std::string &uuid){
    Query q;q.conversation=uuid.c_str();Hold h;
    std::thread active([&]{assert(history(q,Control{0,hold_worker,&h}).ok());});
    while(!h.entered.load())std::this_thread::yield();
    std::vector<std::thread> waiting;
    for(int i=0;i<4;++i)waiting.emplace_back([&]{assert(history(q).ok());});
    for(unsigned i=0;i<10000&&status().queued_requests!=4;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(status().queued_requests==4);assert(history(q).error==Error::busy);
    h.release=true;active.join();for(auto &thread:waiting)thread.join();
}
void long_archive(const Config &config){
    ready(config);std::string uuid=create();assert(valid_uuid(uuid.c_str()));
    assert(record_session_boundary(uuid.c_str(),"provider-session","self-hosted","native-model","start").ok());
    bounded_queue(uuid);
    for(unsigned n=0;n<1000;++n){
        std::string json="{\"id\":\"runtime\",\"messages\":[{\"role\":\"user\",\"content\":\"remember turn "+std::to_string(n)+" marker\"},{\"role\":\"assistant\",\"content\":\"acknowledged turn "+std::to_string(n)+"\",\"reasoning\":{\"text\":\"PRIVATE_REASONING\"}}]}";
        Result r=archive_checkpoint(uuid.c_str(),"provider-session",json.c_str(),n*2);assert(r.ok());
        assert(status().sqlite_highwater<=kSqliteHeapBytes);
    }
    Query q;q.conversation=uuid.c_str();int total=0;
    for(;;){auto j=packet(history(q));auto *a=cJSON_GetObjectItemCaseSensitive(j.get(),"messages");assert(cJSON_GetArraySize(a)<=20);total+=cJSON_GetArraySize(a);int64_t next=static_cast<int64_t>(cJSON_GetObjectItemCaseSensitive(j.get(),"next_cursor")->valuedouble);bool more=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j.get(),"has_more"));if(!more)break;assert(next>q.after_id);q.after_id=next;}
    assert(total==2000);q.after_id=0;q.literal="remember turn 11 marker";assert(rows(search_history(q))==1);
    q.literal="PRIVATE_REASONING";assert(rows(search_history(q))==0);
    q.literal="%";assert(rows(search_history(q))==0); // Literal, not LIKE wildcard.
    q.literal=nullptr;q.count=21;assert(history(q).error==Error::invalid);q.count=20;
    assert(history(q,Control{1,nullptr,nullptr}).error==Error::cancelled);
    assert(history(q,Control{0,cancel,nullptr}).error==Error::cancelled);
    q.byte_limit=2048;auto small=history(q);assert(small.ok()&&small.json.size()<=2048);auto small_page=packet(small);assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(small_page.get(),"has_more")));q.byte_limit=kOutputLimit;
    auto context=recent_context(uuid.c_str(),8192);assert(context.ok()&&context.json.size()<=8192);
    {
        auto restored=packet(restore_session(uuid.c_str()));auto *messages=cJSON_GetObjectItemCaseSensitive(restored.get(),"messages");
        const uint64_t old_count=cJSON_GetArraySize(messages);
        auto *system=cJSON_CreateObject();assert(system);assert(cJSON_AddStringToObject(system,"role","system"));assert(cJSON_AddStringToObject(system,"content","private current instructions"));assert(cJSON_InsertItemInArray(messages,0,system));
        for(const char *role:{"user","assistant"}){auto *message=cJSON_CreateObject();assert(message);assert(cJSON_AddStringToObject(message,"role",role));assert(cJSON_AddStringToObject(message,"content",!strcmp(role,"user")?"new post-resume user marker":"new post-resume assistant marker"));assert(cJSON_AddItemToArray(messages,message));}
        std::unique_ptr<char,decltype(&cJSON_free)> encoded(cJSON_PrintUnformatted(restored.get()),cJSON_free);assert(encoded);
        require_ok(archive_checkpoint(uuid.c_str(),"resumed-session",encoded.get(),0,{},old_count+1),"archive only new resumed messages");
        Query resumed_query;resumed_query.conversation=uuid.c_str();resumed_query.literal="remember turn 999 marker";assert(rows(search_history(resumed_query))==1);
        resumed_query.literal="new post-resume user marker";assert(rows(search_history(resumed_query))==1);
    }
    const char *sanitized=R"({"id":"s","messages":[{"role":"system","content":"PRIVATE_SYSTEM"},{"role":"user","content":{"parts":[{"type":"text","text":"visible multimodal"},{"type":"image","source":{"kind":"base64","data":"PRIVATE_IMAGE"}}]}},{"role":"assistant","content":"","tool_calls":[{"id":"read-call","name":"read_device","arguments":{"value":1}}],"reasoning":{"text":"PRIVATE_REASONING"}},{"role":"tool","call_id":"read-call","name":"read_device","content":"observed read result","is_error":false}]})";
    assert(archive_checkpoint(uuid.c_str(),"provider-session",sanitized,2000).ok());
    assert(archive_checkpoint(uuid.c_str(),"provider-session",sanitized,2000).ok()); // Idempotent finalization.
    {
        auto restored=packet(restore_session(uuid.c_str()));
        auto *messages=cJSON_GetObjectItemCaseSensitive(restored.get(),"messages");const int n=cJSON_GetArraySize(messages);assert(n<=48&&n>=3);
        auto *assistant=cJSON_GetArrayItem(messages,n-2),*tool=cJSON_GetArrayItem(messages,n-1);
        auto *call=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(assistant,"tool_calls"),0);assert(call);
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(call,"id")->valuestring,cJSON_GetObjectItemCaseSensitive(tool,"call_id")->valuestring));
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(tool,"content")->valuestring,"observed read result"));
        assert(restore_session(uuid.c_str(),8192,2).error==Error::too_large); // Never half of the newest three-message group.
        auto native=packet(recent_context(uuid.c_str()));assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(native.get(),"untrusted")));assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(native.get(),"context")));
    }
    q.literal="PRIVATE_";assert(rows(search_history(q))==0);
    q.literal="visible multimodal";assert(rows(search_history(q))==1);
    q.literal=nullptr;q.export_kind=ExportKind::tool_evidence;auto evidence=packet(export_page(q));assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(evidence.get(),"rows"))==1);
    q.export_kind=ExportKind::session_boundaries;auto boundaries=packet(export_page(q));assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(boundaries.get(),"rows"))==1);
    std::string oversized(8193,'x');std::string bad="{\"id\":\"s\",\"messages\":[{\"role\":\"user\",\"content\":\"ROLLBACK_MARKER\"},{\"role\":\"assistant\",\"content\":\""+oversized+"\"}]}";
    assert(!archive_checkpoint(uuid.c_str(),"provider-session",bad.c_str(),2100).ok());q.literal="ROLLBACK_MARKER";assert(rows(search_history(q))==0);
    auto id=id_for(uuid,"completed");auto b=begin_tool(id);assert(b.ok()&&b.decision==Decision::execute);
    assert(begin_tool(id).decision==Decision::inflight);
    const char *result="{\"state\":\"succeeded\",\"observed\":{\"brightness\":42}}";
    auto f=finish_tool(id,Outcome::succeeded,result);assert(f.ok()&&f.outcome==Outcome::succeeded);
    id.arguments_json="{ \"on\": true, \"brightness\": 42.0 }";b=begin_tool(id);assert(b.ok()&&b.decision==Decision::reuse_completed&&b.json==result);
    id.arguments_json="{\"brightness\":43,\"on\":true}";assert(begin_tool(id).decision==Decision::conflict);
    for(Outcome state:{Outcome::failed,Outcome::partial,Outcome::cancelled}) {
        std::string call="terminal-"+std::string(outcome_name(state));auto terminal_id=id_for(uuid,call.c_str());assert(begin_tool(terminal_id).decision==Decision::execute);
        auto terminal=finish_tool(terminal_id,state,"actual non-success evidence",{},"actual terminal error");assert(terminal.ok()&&terminal.outcome==state);
        auto duplicate=begin_tool(terminal_id);assert(duplicate.ok()&&duplicate.decision==Decision::reuse_completed&&duplicate.outcome==state&&duplicate.json=="actual non-success evidence"&&duplicate.tool_error=="actual terminal error");
    }
    auto big=id_for(uuid,"exact-numbers","{\"n\":9007199254740992}");assert(begin_tool(big).decision==Decision::execute);big.arguments_json="{\"n\":9007199254740993}";assert(begin_tool(big).decision==Decision::conflict);
    auto nul=id_for(uuid,"nul","{\"name\":\"a\\u0000b\"}");assert(!begin_tool(nul).ok());
    auto unresolved=id_for(uuid,"uncertain");assert(begin_tool(unresolved).decision==Decision::execute);
    Source s{uuid.c_str(),"provider-session","read-call","source-1","search","request-1","https://example.org/evidence","Evidence","returned source excerpt",{1700000000000LL,0}};
    assert(record_source(s).ok());assert(record_source(s).ok());
    q.literal="observed read result";auto sources=packet(search_history(q));auto *row=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(sources.get(),"messages"),0);assert(row);assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(row,"sources"))==1);
    {
        const char *calls[]={"completed","terminal-failed"};
        Message native{uuid.c_str(),"provider-session","native-final","assistant","native association marker",false,{}};
        native.tool_call_ids=calls;native.tool_call_count=2;assert(append_final_message(native).ok());
        Source source{uuid.c_str(),"provider-session","completed","source-native","search",nullptr,"https://example.org/native","Native evidence","source linked to assistant",{}};assert(record_source(source).ok());
        q.literal="native association marker";auto page=packet(search_history(q));auto *message=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(page.get(),"messages"),0);assert(message);
        auto *tools=cJSON_GetObjectItemCaseSensitive(message,"tools");assert(cJSON_GetArraySize(tools)==2);
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(tools,0),"state")->valuestring,"succeeded"));
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(tools,1),"state")->valuestring,"failed"));
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(tools,1),"error")->valuestring,"actual terminal error"));
        assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(message,"sources"))==1);
        auto *provenance=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(message,"sources"),0);
        assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(provenance,"request_id")));
        assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(provenance,"utc_ms")));
        assert(cJSON_GetObjectItemCaseSensitive(provenance,"monotonic_us")->valuedouble>0);
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(provenance,"call_id")->valuestring,"completed"));
        const char *missing[]={"not-started"};native.item_id="native-missing";native.text="missing ledger must rollback";native.tool_call_ids=missing;native.tool_call_count=1;
        assert(!append_final_message(native).ok());q.literal="missing ledger must rollback";assert(rows(search_history(q))==0);
    }
    const char *pending=R"({"id":"s","messages":[{"role":"user","content":"pending observed action"},{"role":"assistant","content":"","tool_calls":[{"id":"uncertain","name":"niko_control","arguments":{"on":true,"brightness":42}}]}]})";
    assert(archive_checkpoint(uuid.c_str(),"provider-session",pending,2100).ok());
    auto pending_restore=packet(restore_session(uuid.c_str()));auto *pending_rows=cJSON_GetObjectItemCaseSensitive(pending_restore.get(),"messages");
    bool found_unknown=false;
    cJSON *restored_message; cJSON_ArrayForEach(restored_message,pending_rows){auto *role=cJSON_GetObjectItemCaseSensitive(restored_message,"role");if(cJSON_IsString(role)&&!strcmp(role->valuestring,"tool_unknown"))found_unknown=true;}
    assert(found_unknown);
    auto *notice=cJSON_GetArrayItem(pending_rows,cJSON_GetArraySize(pending_rows)-1);auto *notice_text=cJSON_GetObjectItemCaseSensitive(notice,"content");
    assert(cJSON_IsString(notice_text)&&strstr(notice_text->valuestring,"exact-numbers")&&strstr(notice_text->valuestring,unresolved.call_id));
    std::string boot=status().boot_uuid;shutdown();ready(config);assert(boot!=status().boot_uuid);assert(uuid==status().active_uuid);assert(resume(uuid.c_str()).ok());
    b=begin_tool(unresolved);assert(b.ok()&&b.decision==Decision::unknown&&b.outcome==Outcome::unknown);
    id.arguments_json="{\"brightness\":42,\"on\":true}";assert(begin_tool(id).decision==Decision::reuse_completed);
    q.literal="remember turn 11 marker";assert(rows(search_history(q))==1);
    Query latest;latest.conversation=uuid.c_str();latest.count=8;latest.newest_first=true;latest.public_text_only=true;
    auto newest=packet(history(latest));auto *recent_messages=cJSON_GetObjectItemCaseSensitive(newest.get(),"messages");assert(cJSON_GetArraySize(recent_messages)==8);
    int64_t previous=INT64_MAX;
    cJSON *recent_message;cJSON_ArrayForEach(recent_message,recent_messages){int64_t current=cJSON_GetObjectItemCaseSensitive(recent_message,"id")->valuedouble;assert(current<previous);previous=current;const char *role=cJSON_GetObjectItemCaseSensitive(recent_message,"role")->valuestring;assert(!strcmp(role,"user")||!strcmp(role,"assistant"));}
    latest.after_id=previous;auto older=packet(history(latest));auto *older_first=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(older.get(),"messages"),0);assert(older_first&&cJSON_GetObjectItemCaseSensitive(older_first,"id")->valuedouble<previous);
    inject_fault(Fault::sync);auto denied_delete=delete_conversation(uuid.c_str());assert(!denied_delete.ok()&&denied_delete.error==Error::io&&denied_delete.sqlite_code==SQLITE_IOERR_FSYNC);
    shutdown();inject_fault(Fault::none);ready(config);assert(rows(search_history(q))==1);assert(begin_tool(unresolved).decision==Decision::unknown);
    require_ok(delete_conversation(uuid.c_str()),"delete 1000-turn conversation");
    assert(rows(search_history(q))==0);shutdown();
}
// This is the durable host path behind delegated read-only tools, not the
// speech_live transport: its separate 64-ID admission ceiling is unchanged.
void delegation_ledger_over_64(Config config,const std::string &base){
    std::string path=base+"/delegations.sqlite3";config.path=path.c_str();
    ready(config);const std::string uuid=create();
    Message message{uuid.c_str(),"codex-host-session","seed","user","delegation host marker",false,{}};
    assert(append_final_message(message).ok());
    Query query;query.conversation=uuid.c_str();query.literal="delegation host marker";
    std::vector<std::string> results;
    auto identity=[&](const std::string &delegation,const std::string &call){
        ToolIdentity id;id.conversation=uuid.c_str();id.provider_session="codex-host-session";
        id.run_id=delegation.c_str();id.call_id=call.c_str();id.generation=7;
        id.name="search_history";id.arguments_json=R"({"query":"delegation host marker"})";
        return id;
    };
    for(unsigned n=0;n<80;++n){
        const std::string delegation="delegation-"+std::to_string(n);
        const std::string call=delegation+"/read";
        auto id=identity(delegation,call);
        auto admitted=begin_tool(id);assert(admitted.ok()&&admitted.decision==Decision::execute);
        auto observed=search_history(query);assert(rows(observed)==1);
        results.push_back(observed.json);
        if(n==79){
            // The read happened, but no terminal acknowledgment survived.
            auto duplicate=begin_tool(id);assert(duplicate.ok()&&duplicate.decision==Decision::inflight);
        }else{
            auto terminal=finish_tool(id,Outcome::succeeded,observed.json.c_str());
            assert(terminal.ok()&&terminal.outcome==Outcome::succeeded);
        }
    }
    // A replayed search would now return different data. Cached results must
    // remain byte-exact, including the earliest ID after crossing 64.
    message.item_id="later";assert(append_final_message(message).ok());
    assert(rows(search_history(query))==2);
    for(unsigned recovery=0;recovery<2;++recovery){
        if(recovery){shutdown();ready(config);}
        for(unsigned n=0;n<80;++n){
            const std::string delegation="delegation-"+std::to_string(n);
            const std::string call=delegation+"/read";
            auto id=identity(delegation,call);
            auto duplicate=begin_tool(id);assert(duplicate.ok());
            if(n==79){
                assert(duplicate.decision==(recovery?Decision::unknown:Decision::inflight));
                assert(duplicate.outcome==(recovery?Outcome::unknown:Outcome::running));
            }else{
                assert(duplicate.decision==Decision::reuse_completed);
                assert(duplicate.outcome==Outcome::succeeded&&duplicate.json==results[n]);
            }
            id.arguments_json=R"({"query":"different request"})";
            auto conflict=begin_tool(id);
            assert(conflict.error==Error::conflict&&conflict.decision==Decision::conflict);
        }
    }
    shutdown();
    printf("delegation ledger host regression: 80 distinct read-only IDs, 79 exact cached results, conflicts refused, unfinished read unknown after recovery; speech_live 64-ID transport ceiling unchanged (not exercised)\n");
}
void faults(Config c,const std::string &base){
    for(Fault fault:{Fault::short_write,Fault::sync,Fault::full}){
        std::string p=base+"/fault-"+std::to_string(int(fault))+".sqlite3";c.path=p.c_str();ready(c);std::string uuid=create();
        Message before{uuid.c_str(),"s","before","user","committed before fault",false,{}};assert(append_final_message(before).ok());
        inject_fault(fault);Message lost{uuid.c_str(),"s","fault","user","MUST_NOT_ACK",false,{}};
        Result r=append_final_message(lost);assert(!r.ok());if(fault==Fault::full)assert(r.error==Error::full);
        shutdown();inject_fault(Fault::none);ready(c);Query q;q.conversation=uuid.c_str();q.literal="committed before fault";assert(rows(search_history(q))==1);
        q.literal="MUST_NOT_ACK";assert(rows(search_history(q))==0);shutdown();
    }
    {
        std::string terminal_path=base+"/terminal-sync.sqlite3";c.path=terminal_path.c_str();ready(c);std::string uuid=create();auto id=id_for(uuid,"terminal-uncertain");
        assert(begin_tool(id).decision==Decision::execute);inject_fault(Fault::sync);
        auto terminal=finish_tool(id,Outcome::succeeded,"actual observed result");assert(!terminal.ok()&&terminal.outcome==Outcome::unknown);
        shutdown();inject_fault(Fault::none);ready(c);assert(begin_tool(id).decision==Decision::unknown);shutdown();
    }
    {
        std::string read_path=base+"/short-read.sqlite3";c.path=read_path.c_str();ready(c);std::string uuid=create();
        std::string payload(2000,'r');
        for(int n=0;n<80;++n){std::string item=std::to_string(n);Message m{uuid.c_str(),"s",item.c_str(),"user",payload.c_str(),false,{}};assert(append_final_message(m).ok());}
        // Old pages exceed the production 24KiB page cache; this query must read disk.
        inject_fault(Fault::short_read);Query q;q.conversation=uuid.c_str();Result failed=history(q);assert(!failed.ok());
        shutdown();inject_fault(Fault::none);ready(c);assert(rows(history(q))==20);shutdown();
    }
    std::string p=base+"/ceiling.sqlite3";c.path=p.c_str();c.max_database_bytes=128*1024;ready(c);std::string uuid=create();
    std::string text(8000,'x');bool full=false;for(int n=0;n<200;++n){std::string item=std::to_string(n);Message m{uuid.c_str(),"s",item.c_str(),"user",text.c_str(),false,{}};Result r=append_final_message(m);if(!r.ok()){assert(r.error==Error::full);full=true;break;}}
    assert(full);assert(status().sqlite_highwater<=kSqliteHeapBytes);Query q;q.conversation=uuid.c_str();assert(rows(history(q))>0);assert(delete_conversation(uuid.c_str()).ok());assert(status().state==State::ready);assert(!create().empty());shutdown();
}
void crash_recovery(Config c,const std::string &base){
    std::string p=base+"/crash.sqlite3";c.path=p.c_str();ready(c);std::string uuid=create();shutdown();
    pid_t child=fork();assert(child>=0);if(!child){ready(c);auto id=id_for(uuid,"power-loss-after-started");auto r=begin_tool(id);if(!r.ok()||r.decision!=Decision::execute)_exit(2);_exit(0);}
    int code;assert(waitpid(child,&code,0)==child);assert(WIFEXITED(code)&&WEXITSTATUS(code)==0);ready(c);auto id=id_for(uuid,"power-loss-after-started");auto r=begin_tool(id);assert(r.ok()&&r.decision==Decision::unknown);shutdown();
}
int main(){char directory[]="/tmp/arvel-store-XXXXXX";char *base=mkdtemp(directory);assert(base);std::string p=std::string(base)+"/history.sqlite3";Config c;c.path=p.c_str();c.mount_sd=false;c.free_reserve_bytes=0;long_archive(c);faults(c,base);crash_recovery(c,base);delegation_ledger_over_64(c,base);std::filesystem::remove_all(base);printf("conversation_store: 1000-turn archive, ledger, crash recovery, bounded queue/queries/heap and I/O failures passed\n");return 0;}
