#include "conversation_store.hpp"
#include "checked_vfs.hpp"
#include "sqlite3.h"
#include "cJSON.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cstdlib>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#ifdef CONVERSATION_STORE_HOST
#include <thread>
#include <sys/statvfs.h>
#include <openssl/sha.h>
#else
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sdcard_bsp.h"
#include "mbedtls/sha256.h"
#endif

namespace conversation_store {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool bounded(const char *s, size_t n, bool empty = true) { return s && (empty || *s) && strnlen(s, n + 1) <= n; }
bool identity(const char *s) { return bounded(s, 160, false); }
bool role_ok(const char *r) { return r && (!strcmp(r,"user") || !strcmp(r,"assistant") || !strcmp(r,"tool") || !strcmp(r,"tool_unknown")); }
Result error(Error e, int code = 0) { Result r; r.error = e; r.sqlite_code = code; return r; }
std::string quote(const char *s) {
    std::string out = "\"";
    if (s) for (const unsigned char *p = reinterpret_cast<const unsigned char *>(s); *p; ++p) {
        if (*p == '"' || *p == '\\') { out += '\\'; out += *p; }
        else if (*p < 32) { char x[7]; snprintf(x,sizeof(x),"\\u%04x",*p); out += x; }
        else out += *p;
    }
    out += '"'; return out;
}
bool json_budget(const char *s, size_t bytes, unsigned nodes) {
    if (!bounded(s, bytes, false)) return false;
    unsigned depth = 0, count = 0; bool quoted = false, escape = false;
    for (const char *p = s; *p; ++p) {
        if (quoted) { if (escape) escape = false; else if (*p == '\\') { if(!strncmp(p,"\\u0000",6))return false; escape = true; } else if (*p == '"') quoted = false; continue; }
        if (*p == '"') { quoted = true; if (++count > nodes) return false; }
        if (*p == '[' || *p == '{') { if (++depth > 20 || ++count > nodes) return false; }
        if (*p == ']' || *p == '}') { if (!depth--) return false; }
        if (*p == ',' && ++count > nodes) return false;
    }
    return !quoted && depth == 0;
}
// Retain exact decimal identity instead of hashing cJSON's rounded IEEE-754
// values (9007199254740992 and 9007199254740993 MUST conflict).
bool exact_numbers(cJSON *node,const char *&cursor) {
    if(cJSON_IsNumber(node)) {
        bool quoted=false,escaped=false;
        while(*cursor) {
            char c=*cursor;
            if(quoted){++cursor;if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"')quoted=false;continue;}
            if(c=='"'){quoted=true;++cursor;continue;}
            if(c=='-'||(c>='0'&&c<='9'))break;
            ++cursor;
        }
        bool negative=*cursor=='-';if(negative)++cursor;
        std::string coefficient;int exponent=0;
        while(*cursor>='0'&&*cursor<='9')coefficient+=*cursor++;
        if(*cursor=='.'){++cursor;while(*cursor>='0'&&*cursor<='9'){coefficient+=*cursor++;--exponent;}}
        if(*cursor=='e'||*cursor=='E') {
            ++cursor;bool minus=*cursor=='-';if(*cursor=='-'||*cursor=='+')++cursor;
            int power=0,digits=0;while(*cursor>='0'&&*cursor<='9'){if(++digits>6)return false;power=power*10+(*cursor++-'0');}
            exponent+=minus?-power:power;
        }
        size_t first=coefficient.find_first_not_of('0');
        if(first==std::string::npos){coefficient="0";negative=false;exponent=0;}
        else {coefficient.erase(0,first);while(coefficient.size()>1&&coefficient.back()=='0'){coefficient.pop_back();++exponent;}}
        std::string value=(negative?"-":"")+coefficient;
        if(exponent){value+='e';value+=std::to_string(exponent);}
        node->valuestring=static_cast<char *>(cJSON_malloc(value.size()+1));if(!node->valuestring)return false;
        memcpy(node->valuestring,value.c_str(),value.size()+1);
    }
    for(cJSON *child=node->child;child;child=child->next)if(!exact_numbers(child,cursor))return false;
    return true;
}
Json parse(const char *s, size_t cap, unsigned nodes = 2048) {
    if (!json_budget(s, cap, nodes)) return Json(nullptr,cJSON_Delete);
    const char *end = nullptr;
    Json j(cJSON_ParseWithLengthOpts(s, strlen(s)+1, &end, 1),cJSON_Delete);
    const char *numbers=s;if(j&&!exact_numbers(j.get(),numbers))j.reset();
    return j;
}
const char *str(cJSON *j, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j,key); return cJSON_IsString(v) ? v->valuestring : nullptr;
}
bool sensitive(const char *s) {
    return s && (!strcmp(s,"authorization") || !strcmp(s,"Authorization") || !strcmp(s,"access_token") ||
        !strcmp(s,"refresh_token") || !strcmp(s,"api_key") || !strcmp(s,"password") || !strcmp(s,"secret") ||
        !strcmp(s,"reasoning") || !strcmp(s,"chain_of_thought"));
}
bool canonical(cJSON *j, std::string &out, unsigned depth = 0, bool redact = false) {
    if (!j || depth > 20 || out.size() > kTextLimit) return false;
    if (cJSON_IsObject(j)) {
        std::vector<cJSON *> members;
        for (cJSON *p=j->child;p;p=p->next) { if (!p->string) return false; members.push_back(p); }
        std::sort(members.begin(),members.end(),[](cJSON *a,cJSON *b){return strcmp(a->string,b->string)<0;});
        out += '{'; const char *previous = nullptr;
        for (auto *p:members) {
            if (previous && !strcmp(previous,p->string)) return false;
            if (previous) out += ',';
            previous = p->string;
            out += quote(p->string); out += ':';
            if (redact && sensitive(p->string)) out += "\"[redacted]\"";
            else if (!canonical(p,out,depth+1,redact)) return false;
        }
        out += '}';
    } else if (cJSON_IsArray(j)) {
        out += '[';
        for (auto *p=j->child;p;p=p->next) { if (p != j->child) out += ','; if (!canonical(p,out,depth+1,redact)) return false; }
        out += ']';
    } else if(cJSON_IsNumber(j)&&j->valuestring) {
        out+=j->valuestring;
    } else {
        char *printed=cJSON_PrintUnformatted(j); if (!printed) return false;
        out += printed; cJSON_free(printed);
    }
    return out.size() <= kTextLimit;
}
std::string hash(const std::string &s) {
    unsigned char bytes[32];
#ifdef CONVERSATION_STORE_HOST
    SHA256(reinterpret_cast<const unsigned char *>(s.data()),s.size(),bytes);
#else
    if (mbedtls_sha256(reinterpret_cast<const unsigned char *>(s.data()),s.size(),bytes,0)) return {};
#endif
    char out[65]; for (unsigned n=0;n<32;++n) snprintf(out+2*n,3,"%02x",bytes[n]); return out;
}
bool uuid(char *out) {
    unsigned char b[16]; if(!internal::random(b,sizeof(b)))return false; b[6]=(b[6]&15)|64; b[8]=(b[8]&63)|128;
    snprintf(out,37,"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
    return true;
}
enum class Op { create, close, resume, append, boundary, checkpoint, list, history, search, export_rows, recent, restore, erase, source, begin, finish };
struct Request {
    Op op;
    const void *data;
    Control control;
    Result result;
    std::string tool_error;
    Decision decision = Decision::rejected;
    Outcome outcome = Outcome::unknown;
    bool done = false;
    std::condition_variable completed;
    Request(Op operation,const void *payload,Control cancellation)
        : op(operation),data(payload),control(cancellation) {}
};
struct Checkpoint { const char *uuid; const char *session; const char *json; uint64_t offset; uint64_t archive_from_offset; };
struct Boundary { const char *uuid; const char *session; const char *provider; const char *model; const char *reason; Time time; };
struct Recent { const char *uuid; size_t limit; unsigned max_messages=48; };
struct Finish { const ToolIdentity *id; Outcome outcome; const char *json; const char *error_text; };
struct List { int64_t cursor; unsigned count; };
std::mutex gate;
std::condition_variable work, drained;
std::array<Request *,4> queue{};
size_t queued = 0;
bool started = false, stopping = false, exited = true;
Status snapshot;
char path[240] = {};
Config config;
sqlite3 *db = nullptr;
void *heap = nullptr;
Request *current = nullptr;
#ifdef CONVERSATION_STORE_HOST
std::thread worker;
#else
TaskHandle_t worker = nullptr;
#endif
bool cancelled() {
    return current && ((current->control.deadline_us && now_us() >= current->control.deadline_us) ||
        (current->control.cancelled && current->control.cancelled(current->control.context)));
}
int progress(void *) {
#ifndef CONVERSATION_STORE_HOST
    // This worker shares the live-voice priority; yield periodically so long
    // archive work cannot starve LVGL or the idle task (task watchdog).
    static uint64_t yielded = 0;
    if (now_us() - yielded > 100000) { vTaskDelay(1); yielded = now_us(); }
#endif
    return cancelled() ? 1 : 0;
}
void state(State value, int code = 0) {
    std::lock_guard<std::mutex> lock(gate); snapshot.state=value; snapshot.sqlite_code=code;
}
void active(const char *id) { std::lock_guard<std::mutex> lock(gate); snprintf(snapshot.active_uuid,sizeof(snapshot.active_uuid),"%s",id?id:""); }
std::string active_id() { std::lock_guard<std::mutex> lock(gate); return snapshot.active_uuid; }
Result sql_error(int rc) {
    const int base=rc&255;
    if (base==SQLITE_INTERRUPT) return error(Error::cancelled,rc);
    if (base==SQLITE_FULL) { state(State::full,rc); return error(Error::full,rc); }
    if (base==SQLITE_BUSY || base==SQLITE_LOCKED) return error(Error::busy,rc);
    if (base==SQLITE_CONSTRAINT) return error(Error::conflict,rc);
    if (base==SQLITE_TOOBIG || base==SQLITE_NOMEM) return error(Error::too_large,rc);
    state(State::error,rc); return error(Error::io,rc);
}
struct Statement {
    sqlite3_stmt *p=nullptr;
    int rc;
    explicit Statement(const char *sql):rc(sqlite3_prepare_v2(db,sql,-1,&p,nullptr)) {}
    ~Statement() { if(p) sqlite3_finalize(p); }
    void text(int n,const char *s) { if(rc==SQLITE_OK) rc=s?sqlite3_bind_text(p,n,s,-1,SQLITE_TRANSIENT):sqlite3_bind_null(p,n); }
    void number(int n,int64_t v) { if(rc==SQLITE_OK) rc=sqlite3_bind_int64(p,n,v); }
    int step() { return rc==SQLITE_OK ? sqlite3_step(p) : rc; }
    const char *column(int n) { const auto *v=sqlite3_column_text(p,n); return v?reinterpret_cast<const char *>(v):""; }
};
int exec(const char *sql) { return sqlite3_exec(db,sql,nullptr,nullptr,nullptr); }
struct Transaction {
    int rc=exec("BEGIN IMMEDIATE"); bool committed=false;
    ~Transaction() {
        if (!committed && rc==SQLITE_OK && !sqlite3_get_autocommit(db)) {
            // Cancellation must not interrupt rollback and leave a transaction open.
            sqlite3_progress_handler(db,0,nullptr,nullptr);
            int rollback=exec("ROLLBACK");
            sqlite3_progress_handler(db,1000,progress,nullptr);
            if (rollback!=SQLITE_OK) state(State::error,rollback);
        }
    }
    int commit() { if(rc!=SQLITE_OK)return rc; if(cancelled())return SQLITE_INTERRUPT; int r=exec("COMMIT"); committed=r==SQLITE_OK; return r; }
};
void bind_time(Statement &s,int first,Time time) {
    if(!time.utc_ms&&config.trusted_utc_now_ms)time.utc_ms=config.trusted_utc_now_ms();
    if(time.utc_ms>=1577836800000LL) s.number(first,time.utc_ms); else if(s.rc==SQLITE_OK)s.rc=sqlite3_bind_null(s.p,first);
    s.number(first+1,time.monotonic_us?time.monotonic_us:now_us()); s.text(first+2,snapshot.boot_uuid);
}
bool writable() { std::lock_guard<std::mutex> lock(gate); return snapshot.state==State::ready; }
uint64_t db_size() { struct stat st; return stat(path,&st)==0?static_cast<uint64_t>(st.st_size):0; }
Result reserve_space(bool allow_oversize=false) {
    uint64_t available=0;
#ifdef CONVERSATION_STORE_HOST
    struct statvfs st; if(statvfs(path,&st))return sql_error(SQLITE_IOERR_FSTAT);
    available=static_cast<uint64_t>(st.f_bavail)*st.f_frsize;
#else
    uint64_t total=0; if(esp_vfs_fat_info("/sdcard",&total,&available)!=ESP_OK)return sql_error(SQLITE_IOERR_FSTAT);
#endif
    if(!allow_oversize&&db_size()>config.max_database_bytes)return sql_error(SQLITE_FULL);
    if(available<config.free_reserve_bytes+db_size()+128*1024) return sql_error(SQLITE_FULL);
    return {};
}
Result space(bool allow_full=false) {
    if(!writable()){std::lock_guard<std::mutex>lock(gate);if(!allow_full||snapshot.state!=State::full)return error(Error::unavailable);}
    return reserve_space(allow_full);
}
Result write_message(const Message &m) {
    if(!valid_uuid(m.conversation)||!identity(m.provider_session)||!identity(m.item_id)||!role_ok(m.role)||!bounded(m.text,kTextLimit))return error(Error::invalid);
    const bool tool=!strcmp(m.role,"tool")||!strcmp(m.role,"tool_unknown");
    if(tool&&(!identity(m.call_id)||!identity(m.tool_name)))return error(Error::invalid);
    if(m.tool_call_count>16||(m.tool_call_count&&(!m.tool_call_ids||strcmp(m.role,"assistant"))))return error(Error::invalid);
    Statement s("INSERT INTO messages(conversation,session,item,role,text,interrupted,utc_ms,mono_us,boot,call_id,tool_name,is_error) VALUES(?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(conversation,session,item) DO UPDATE SET interrupted=MAX(messages.interrupted,excluded.interrupted) WHERE messages.role=excluded.role AND messages.text=excluded.text AND messages.call_id IS excluded.call_id AND messages.tool_name IS excluded.tool_name AND messages.is_error=excluded.is_error");
    s.text(1,m.conversation);s.text(2,m.provider_session);s.text(3,m.item_id);s.text(4,m.role);s.text(5,m.text);s.number(6,m.interrupted);bind_time(s,7,m.time);s.text(10,tool?m.call_id:nullptr);s.text(11,tool?m.tool_name:nullptr);s.number(12,tool&&m.is_error);
    int rc=s.step(); if(rc!=SQLITE_DONE)return sql_error(rc);
    if(sqlite3_changes(db)!=1)return error(Error::conflict);
    for(unsigned index=0;index<m.tool_call_count;++index) {
        const char *call=m.tool_call_ids[index];if(!identity(call))return error(Error::invalid);
        Statement link("INSERT INTO tool_evidence(conversation,session,call_id,name,arguments,message_id) SELECT t.conversation,t.session,t.call_id,t.name,t.arguments,m.id FROM tool_calls t JOIN messages m ON m.conversation=t.conversation AND m.session=t.session WHERE t.conversation=? AND t.session=? AND t.call_id=? AND m.item=? ON CONFLICT(conversation,session,call_id) DO NOTHING");
        link.text(1,m.conversation);link.text(2,m.provider_session);link.text(3,call);link.text(4,m.item_id);int linked=link.step();if(linked!=SQLITE_DONE)return sql_error(linked);
        Statement verify("SELECT 1 FROM tool_evidence e JOIN messages m ON e.message_id=m.id WHERE e.conversation=? AND e.session=? AND e.call_id=? AND m.item=?");
        verify.text(1,m.conversation);verify.text(2,m.provider_session);verify.text(3,call);verify.text(4,m.item_id);linked=verify.step();if(linked!=SQLITE_ROW)return linked==SQLITE_DONE?error(Error::not_found):sql_error(linked);
    }
    return {};
}
Result create_conversation(const char *title) {
    if(!bounded(title,160))return error(Error::invalid);
    char id[37];if(!uuid(id))return error(Error::io);
    Transaction tx;if(tx.rc!=SQLITE_OK)return sql_error(tx.rc);
    Statement close("UPDATE conversations SET closed=1 WHERE uuid=?");const auto previous=active_id();close.text(1,previous.c_str());int rc=close.step();if(rc!=SQLITE_DONE)return sql_error(rc);
    Statement s("INSERT INTO conversations(uuid,title,utc_ms,mono_us,boot) VALUES(?,?,?,?,?)");s.text(1,id);s.text(2,title);bind_time(s,3,{});
    rc=s.step();if(rc!=SQLITE_DONE)return sql_error(rc);rc=tx.commit();if(rc!=SQLITE_OK)return sql_error(rc);
    active(id);Result r;r.json="{\"uuid\":"+quote(id)+"}";return r;
}
Result lifecycle(const char *id,bool close) {
    if(!valid_uuid(id))return error(Error::invalid);
    Transaction tx;if(tx.rc!=SQLITE_OK)return sql_error(tx.rc);
    if(!close){Statement previous("UPDATE conversations SET closed=1 WHERE uuid=?");const auto old=active_id();previous.text(1,old.c_str());int rc=previous.step();if(rc!=SQLITE_DONE)return sql_error(rc);}
    Statement s(close?"UPDATE conversations SET closed=1 WHERE uuid=?":"UPDATE conversations SET closed=0 WHERE uuid=?");s.text(1,id);int rc=s.step();if(rc!=SQLITE_DONE)return sql_error(rc);if(!sqlite3_changes(db))return error(Error::not_found);
    rc=tx.commit();if(rc!=SQLITE_OK)return sql_error(rc);if(close){if(active_id()==id)active("");}else active(id);
    Result r;r.json="{\"uuid\":"+quote(id)+"}";return r;
}
Result boundary(const Boundary &b) {
    if(!valid_uuid(b.uuid)||!identity(b.session)||!bounded(b.provider,160)||!bounded(b.model,160)||!bounded(b.reason,160))return error(Error::invalid);
    Transaction tx;if(tx.rc!=SQLITE_OK)return sql_error(tx.rc);
    Statement s("INSERT INTO sessions(conversation,session,provider,model,reason,utc_ms,mono_us,boot) VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(conversation,session,reason) DO NOTHING");
    s.text(1,b.uuid);s.text(2,b.session);s.text(3,b.provider);s.text(4,b.model);s.text(5,b.reason);bind_time(s,6,b.time);int rc=s.step();if(rc!=SQLITE_DONE)return sql_error(rc);rc=tx.commit();return rc==SQLITE_OK?Result{}:sql_error(rc);
}
Result checkpoint(const Checkpoint &c) {
    if(!valid_uuid(c.uuid)||!identity(c.session)||c.offset>INT64_MAX-128||c.archive_from_offset>INT64_MAX)return error(Error::invalid);
    Json root=parse(c.json,64*1024);if(!root)return error(Error::invalid);
    cJSON *messages=cJSON_GetObjectItemCaseSensitive(root.get(),"messages");if(!cJSON_IsArray(messages)||cJSON_GetArraySize(messages)>128)return error(Error::invalid);
    Transaction tx;if(tx.rc!=SQLITE_OK)return sql_error(tx.rc);
    unsigned index=0;
    for(auto *m=messages->child;m;m=m->next,++index) {
        if(cancelled())return error(Error::cancelled);
        if(c.offset+index<c.archive_from_offset)continue;
        const char *role=str(m,"role");if(role&&!strcmp(role,"system"))continue;
        if(!role_ok(role))return error(Error::invalid);
        std::string text;const char *content=str(m,"content");
        if(content)text=content;
        else if(!strcmp(role,"user")) {
            cJSON *object=cJSON_GetObjectItemCaseSensitive(m,"content");cJSON *parts=cJSON_GetObjectItemCaseSensitive(object,"parts");
            if(!cJSON_IsArray(parts))return error(Error::invalid);
            for(auto *p=parts->child;p;p=p->next) {const char *kind=str(p,"type"),*part=str(p,"text");if(kind&&!strcmp(kind,"text")&&part){if(!text.empty())text+='\n';text+=part;if(text.size()>kTextLimit)return error(Error::too_large);}}
        } else if(!strcmp(role,"tool_unknown")) { const char *reason=str(m,"reason");if(!reason)return error(Error::invalid);text=reason; }
        else if(strcmp(role,"assistant")) return error(Error::invalid);
        if(text.size()>kTextLimit)return error(Error::too_large);
        // Only whitelisted public fields are materialized. Never serialize the Session or its reasoning/images.
        char item[64];snprintf(item,sizeof(item),"text:%llu",static_cast<unsigned long long>(c.offset+index));
        Message value{c.uuid,c.session,item,role,text.c_str(),false,{}};
        const bool tool=!strcmp(role,"tool")||!strcmp(role,"tool_unknown");
        if(tool){value.call_id=str(m,"call_id");value.tool_name=str(m,"name");value.is_error=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m,"is_error"));}
        // Every checkpoint resends the whole session window. Rows archived by an
        // earlier committed checkpoint are skipped: rewriting them only adds
        // journaled, fsynced SD writes that grow with the conversation.
        Statement same("SELECT 1 FROM messages WHERE conversation=? AND session=? AND item=? AND role=? AND text=? AND interrupted=0 AND call_id IS ? AND tool_name IS ? AND is_error=?");
        same.text(1,c.uuid);same.text(2,c.session);same.text(3,item);same.text(4,role);same.text(5,text.c_str());
        same.text(6,tool?value.call_id:nullptr);same.text(7,tool?value.tool_name:nullptr);same.number(8,tool&&value.is_error);
        const int archived=same.step();if(archived==SQLITE_ROW)continue;if(archived!=SQLITE_DONE)return sql_error(archived);
        Result r=write_message(value);if(!r.ok())return r;
        cJSON *calls=cJSON_GetObjectItemCaseSensitive(m,"tool_calls");
        if(calls) {
            if(strcmp(role,"assistant")||!cJSON_IsArray(calls)||cJSON_GetArraySize(calls)>16)return error(Error::invalid);
            for(auto *call=calls->child;call;call=call->next) {
                const char *id=str(call,"id"),*name=str(call,"name");cJSON *args=cJSON_GetObjectItemCaseSensitive(call,"arguments");
                if(!identity(id)||!identity(name)||!cJSON_IsObject(args))return error(Error::invalid);
                std::string evidence;if(!canonical(args,evidence,0,true))return error(Error::too_large);
                Statement e("INSERT INTO tool_evidence(conversation,session,call_id,name,arguments,message_id) SELECT ?,?,?,?,?,id FROM messages WHERE conversation=? AND session=? AND item=? ON CONFLICT(conversation,session,call_id) DO NOTHING");
                e.text(1,c.uuid);e.text(2,c.session);e.text(3,id);e.text(4,name);e.text(5,evidence.c_str());e.text(6,c.uuid);e.text(7,c.session);e.text(8,item);int rc=e.step();if(rc!=SQLITE_DONE)return sql_error(rc);
            }
        }
    }
    int rc=tx.commit();return rc==SQLITE_OK?Result{}:sql_error(rc);
}
std::string source_packet(int64_t message_id) {
    Statement s("SELECT source_id,provider,request_id,url,title,excerpt,utc_ms,mono_us,boot,call_id,session,conversation FROM sources WHERE conversation=(SELECT conversation FROM messages WHERE id=?) AND session=(SELECT session FROM messages WHERE id=?) AND (call_id=(SELECT call_id FROM messages WHERE id=?) OR call_id IN (SELECT call_id FROM tool_evidence WHERE message_id=?)) ORDER BY id LIMIT 4");
    s.number(1,message_id);s.number(2,message_id);s.number(3,message_id);s.number(4,message_id);std::string out="[";int rc;
    while((rc=s.step())==SQLITE_ROW) {
        if(out.size()>1)out+=',';
        out+="{\"id\":"+quote(s.column(0))+",\"provider\":"+quote(s.column(1))+",\"request_id\":"+(*s.column(2)?quote(s.column(2)):std::string("null"))+",\"url\":"+quote(s.column(3))+",\"title\":"+quote(s.column(4))+",\"excerpt\":"+quote(s.column(5))+",\"utc_ms\":";
        out+=sqlite3_column_type(s.p,6)==SQLITE_NULL?"null":std::to_string(sqlite3_column_int64(s.p,6));
        out+=",\"monotonic_us\":"+std::to_string(sqlite3_column_int64(s.p,7))+",\"boot_uuid\":"+quote(s.column(8))+",\"call_id\":"+quote(s.column(9))+",\"provider_session\":"+quote(s.column(10))+",\"conversation\":"+quote(s.column(11))+"}";
    }
    if(rc!=SQLITE_DONE)return {};
    out+=']';
    return out;
}
std::string tool_packet(int64_t message_id) {
    Statement s("SELECT e.call_id,e.name,c.state,c.result,c.error FROM tool_evidence e LEFT JOIN tool_calls c ON c.conversation=e.conversation AND c.session=e.session AND c.call_id=e.call_id WHERE e.message_id=? ORDER BY e.id LIMIT 17");
    s.number(1,message_id);std::string out="[";unsigned count=0;int rc;
    while((rc=s.step())==SQLITE_ROW) {
        if(count==16)return {};
        const char *state=s.column(2);if(!*state)state="unknown"; // Evidence is not a durable outcome acknowledgment.
        std::string failure=s.column(4);
        if(strcmp(state,"succeeded")&&failure.empty()) {
            Json result=parse(s.column(3),kTextLimit,512);
            const char *recorded=result?str(result.get(),"error"):nullptr;
            if(recorded)failure=recorded;
            else if(!result&&*s.column(3))failure=s.column(3);
            if(failure.size()>1024){size_t cut=1024;while(cut&&(static_cast<unsigned char>(failure[cut])&0xc0)==0x80)--cut;failure.resize(cut);}
        }
        if(count++)out+=',';
        out+="{\"id\":"+quote(s.column(0))+",\"kind\":\"tool\",\"verb\":"+quote(s.column(1))+",\"target\":\"\",\"state\":"+quote(state)+",\"error\":"+quote(failure.c_str())+"}";
    }
    if(rc!=SQLITE_DONE)return {};
    out+=']';
    return out;
}
Result page(const Query &q,bool search) {
    if((q.conversation&&!valid_uuid(q.conversation))||q.after_id<0||q.count<1||q.count>kPageLimit||(q.literal&&!bounded(q.literal,256))||q.utc_from_ms<0||q.utc_to_ms<0)return error(Error::invalid);
    if(q.byte_limit<256||q.byte_limit>kOutputLimit)return error(Error::invalid);
    if(search&&(!q.literal||!*q.literal))return error(Error::invalid);
    std::string sql="SELECT id,conversation,session,item,role,text,interrupted,utc_ms,mono_us,boot,call_id,tool_name FROM messages WHERE ";
    sql+=q.newest_first?"id<?":"id>?";
    if(q.public_text_only)sql+=" AND role IN ('user','assistant')";
    if(q.conversation)sql+=" AND conversation=?";
    if(q.literal)sql+=" AND instr(text,?)>0";
    if(q.utc_from_ms)sql+=" AND utc_ms>=?";
    if(q.utc_to_ms)sql+=" AND utc_ms<=?";
    sql+=q.newest_first?" ORDER BY id DESC LIMIT ?":" ORDER BY id LIMIT ?";
    Statement s(sql.c_str());int parameter=1;s.number(parameter++,q.newest_first&&!q.after_id?INT64_MAX:q.after_id);
    if(q.conversation)s.text(parameter++,q.conversation);
    if(q.literal)s.text(parameter++,q.literal);
    if(q.utc_from_ms)s.number(parameter++,q.utc_from_ms);
    if(q.utc_to_ms)s.number(parameter++,q.utc_to_ms);
    s.number(parameter,q.count+1);
    std::string out="{\"untrusted\":true,\"messages\":[";int64_t cursor=q.after_id;unsigned n=0;bool more=false;int rc;
    while((rc=s.step())==SQLITE_ROW) {
        if(n==q.count){more=true;break;}
        int64_t id=sqlite3_column_int64(s.p,0);
        std::string text=s.column(5);
        if(search&&text.size()>1024) {
            const size_t match=q.literal?text.find(q.literal):std::string::npos;
            size_t begin=match!=std::string::npos&&match>256?match-256:0;
            while(begin&&(static_cast<unsigned char>(text[begin])&0xc0)==0x80)--begin;
            size_t end=std::min(text.size(),begin+1024);
            while(end<text.size()&&end>begin&&(static_cast<unsigned char>(text[end])&0xc0)==0x80)--end;
            text=(begin?"…":"")+text.substr(begin,end-begin)+(end<text.size()?"…":"");
        }
        std::string sources=q.include_sources?source_packet(id):"[]";if(sources.empty())return sql_error(sqlite3_errcode(db));
        std::string tools=tool_packet(id);if(tools.empty())return sql_error(SQLITE_IOERR_READ);
        std::string row="{\"id\":"+std::to_string(id)+",\"conversation\":"+quote(s.column(1))+",\"provider_session\":"+quote(s.column(2))+",\"item_id\":"+quote(s.column(3))+",\"role\":"+quote(s.column(4))+",\"text\":"+quote(text.c_str())+",\"interrupted\":"+(sqlite3_column_int(s.p,6)?"true":"false")+",\"utc_ms\":";
        row+=sqlite3_column_type(s.p,7)==SQLITE_NULL?"null":std::to_string(sqlite3_column_int64(s.p,7));
        row+=",\"monotonic_us\":"+std::to_string(sqlite3_column_int64(s.p,8))+",\"boot_uuid\":"+quote(s.column(9))+",\"call_id\":"+quote(s.column(10))+",\"tool_name\":"+quote(s.column(11))+",\"tools\":"+tools+",\"sources\":"+sources+"}";
        if(out.size()+row.size()+128>q.byte_limit){if(!n)return error(Error::too_large);more=true;break;}
        if(n++)out+=',';
        out+=row;
        cursor=id;
    }
    if(rc!=SQLITE_DONE&&rc!=SQLITE_ROW)return sql_error(rc);
    out+="],\"next_cursor\":"+std::to_string(cursor)+",\"has_more\":"+(more?"true":"false")+"}";Result r;r.json=std::move(out);return r;
}
Result list(const List &q) {
    if(q.cursor<0||q.count<1||q.count>kPageLimit)return error(Error::invalid);
    Statement s("SELECT id,uuid,title,closed,utc_ms FROM conversations WHERE id>? ORDER BY id LIMIT ?");s.number(1,q.cursor);s.number(2,q.count+1);
    Result r;r.json="{\"conversations\":[";int64_t cursor=q.cursor;unsigned n=0;bool more=false;int rc;
    while((rc=s.step())==SQLITE_ROW){if(n==q.count){more=true;break;}if(n++)r.json+=',';cursor=sqlite3_column_int64(s.p,0);r.json+="{\"id\":"+std::to_string(cursor)+",\"uuid\":"+quote(s.column(1))+",\"title\":"+quote(s.column(2))+",\"closed\":"+(sqlite3_column_int(s.p,3)?"true":"false")+",\"utc_ms\":"+(sqlite3_column_type(s.p,4)==SQLITE_NULL?"null":std::to_string(sqlite3_column_int64(s.p,4)))+"}";}
    if(rc!=SQLITE_DONE&&rc!=SQLITE_ROW)return sql_error(rc);
    r.json+="],\"next_cursor\":"+std::to_string(cursor)+",\"has_more\":"+(more?"true":"false")+"}";
    return r;
}
Result export_rows(const Query &q) {
    if(q.export_kind==ExportKind::messages)return page(q,false);
    if((q.conversation&&!valid_uuid(q.conversation))||q.after_id<0||q.count<1||q.count>kPageLimit||q.literal||q.utc_from_ms||q.utc_to_ms||q.newest_first||q.public_text_only)return error(Error::invalid);
    if(q.byte_limit<256||q.byte_limit>kOutputLimit)return error(Error::invalid);
    const char *table=nullptr;
    switch(q.export_kind) {
    case ExportKind::session_boundaries:table="sessions";break;
    case ExportKind::tool_calls:table="tool_calls";break;
    case ExportKind::sources:table="sources";break;
    case ExportKind::tool_evidence:table="tool_evidence";break;
    default:return error(Error::invalid);
    }
    std::string sql="SELECT rowid AS cursor,* FROM ";sql+=table;sql+=" WHERE rowid>?";
    if(q.conversation)sql+=" AND conversation=?";
    sql+=" ORDER BY rowid LIMIT ?";
    Statement s(sql.c_str());s.number(1,q.after_id);int parameter=2;if(q.conversation)s.text(parameter++,q.conversation);s.number(parameter,q.count+1);
    Result r;r.json="{\"untrusted\":true,\"kind\":"+quote(table)+",\"rows\":[";
    unsigned n=0;int64_t cursor=q.after_id;bool more=false;int rc;
    while((rc=s.step())==SQLITE_ROW) {
        if(n==q.count){more=true;break;}
        std::string row="{";
        for(int column=0;column<sqlite3_column_count(s.p);++column) {
            if(column)row+=',';
            row+=quote(sqlite3_column_name(s.p,column));
            row+=':';
            const int type=sqlite3_column_type(s.p,column);
            if(type==SQLITE_NULL)row+="null";
            else if(type==SQLITE_INTEGER)row+=std::to_string(sqlite3_column_int64(s.p,column));
            else if(type==SQLITE_TEXT)row+=quote(s.column(column));
            else return error(Error::invalid);
        }
        row+='}';
        if(r.json.size()+row.size()+128>q.byte_limit){if(!n)return error(Error::too_large);more=true;break;}
        if(n++)r.json+=',';
        r.json+=row;
        cursor=sqlite3_column_int64(s.p,0);
    }
    if(rc!=SQLITE_ROW&&rc!=SQLITE_DONE)return sql_error(rc);
    r.json+="],\"next_cursor\":"+std::to_string(cursor)+",\"has_more\":"+(more?"true":"false")+"}";return r;
}
struct ArchivedCall {
    std::string key, name, session, provider_id;
};
struct RestoreRow {
    std::string json, orphan_text, result_key;
    bool user=false, assistant=false;
    std::vector<ArchivedCall> calls;
};
Result encode_restore_row(Statement &message,const char *conversation,RestoreRow &row) {
    const int64_t message_id=sqlite3_column_int64(message.p,0);
    const std::string session=message.column(1),role=message.column(2);
    std::string text=message.column(3);
    row.user=role=="user";row.assistant=role=="assistant";
    if(row.assistant&&sqlite3_column_int(message.p,4))
        text+="\n[Assistant audio was interrupted; this transcript does not prove all words were heard.]";
    if(row.user||row.assistant) {
        row.json="{\"role\":"+quote(role.c_str())+",\"content\":"+quote(text.c_str());
        if(row.assistant) {
            Statement calls("SELECT id,call_id,name,arguments FROM tool_evidence WHERE message_id=? ORDER BY id LIMIT 17");
            calls.number(1,message_id);int rc;
            while((rc=calls.step())==SQLITE_ROW) {
                if(row.calls.size()==16)return error(Error::too_large);
                ArchivedCall call{"archive:"+std::to_string(sqlite3_column_int64(calls.p,0)),calls.column(2),session,calls.column(1)};
                Json arguments=parse(calls.column(3),kTextLimit,512);
                if(!arguments||!cJSON_IsObject(arguments.get()))return error(Error::invalid);
                if(row.calls.empty())row.json+=",\"tool_calls\":[";
                else row.json+=',';
                row.json+="{\"id\":"+quote(call.key.c_str())+",\"name\":"+quote(call.name.c_str())+",\"arguments\":"+calls.column(3)+"}";
                row.calls.push_back(std::move(call));
            }
            if(rc!=SQLITE_DONE)return sql_error(rc);
            if(!row.calls.empty())row.json+=']';
        }
        row.json+='}';return {};
    }
    if(role!="tool"&&role!="tool_unknown")return error(Error::invalid);
    const std::string call_id=message.column(5),name=message.column(6);
    row.orphan_text="{\"role\":\"assistant\",\"content\":"+quote(("[Archived tool evidence, not a new action. "+name+"/"+call_id+"] "+text).c_str())+"}";
    Statement call("SELECT id FROM tool_evidence WHERE conversation=? AND session=? AND call_id=?");
    call.text(1,conversation);call.text(2,session.c_str());call.text(3,call_id.c_str());int rc=call.step();
    if(rc==SQLITE_DONE){row.json=row.orphan_text;return {};}
    if(rc!=SQLITE_ROW)return sql_error(rc);
    row.result_key="archive:"+std::to_string(sqlite3_column_int64(call.p,0));
    row.json="{\"role\":"+quote(role.c_str())+",\"call_id\":"+quote(row.result_key.c_str())+",\"name\":"+quote(name.c_str());
    row.json+=role=="tool_unknown"?",\"reason\":"+quote(text.c_str()):",\"content\":"+quote(text.c_str())+",\"is_error\":"+(sqlite3_column_int(message.p,7)?"true":"false");
    row.json+='}';return {};
}
Result complete_group(const char *conversation,std::vector<RestoreRow> &descending,std::vector<std::string> &out) {
    std::vector<ArchivedCall> pending;
    auto reconcile=[&]() -> Result {
        for(const auto &call:pending) {
            Statement s("SELECT state,result,error FROM tool_calls WHERE conversation=? AND session=? AND call_id=?");
            s.text(1,conversation);s.text(2,call.session.c_str());s.text(3,call.provider_id.c_str());int rc=s.step();
            if(rc!=SQLITE_ROW&&rc!=SQLITE_DONE)return sql_error(rc);
            const std::string state=rc==SQLITE_ROW?s.column(0):"unknown";
            std::string result=rc==SQLITE_ROW?s.column(1):"No archived terminal observation; outcome is unknown. Do not replay this action.";
            if(rc==SQLITE_ROW&&result.empty()&&*s.column(2))result=s.column(2);
            std::string row="{\"role\":";
            if(state=="unknown"||state=="running")
                row+="\"tool_unknown\",\"call_id\":"+quote(call.key.c_str())+",\"name\":"+quote(call.name.c_str())+",\"reason\":"+quote(result.empty()?"No durable terminal acknowledgment; outcome is unknown.":result.c_str())+"}";
            else row+="\"tool\",\"call_id\":"+quote(call.key.c_str())+",\"name\":"+quote(call.name.c_str())+",\"content\":"+quote(result.c_str())+",\"is_error\":"+(state=="succeeded"?"false":"true")+"}";
            out.push_back(std::move(row));
        }
        pending.clear();return {};
    };
    for(auto it=descending.rbegin();it!=descending.rend();++it) {
        auto &row=*it;
        if(row.user||row.assistant){Result r=reconcile();if(!r.ok())return r;}
        if(!row.result_key.empty()) {
            auto match=std::find_if(pending.begin(),pending.end(),[&](const ArchivedCall &call){return call.key==row.result_key;});
            if(match==pending.end())row.json=std::move(row.orphan_text);
            else pending.erase(match);
        }
        out.push_back(std::move(row.json));
        for(auto &call:row.calls)pending.push_back(std::move(call));
    }
    return reconcile();
}
Result unresolved_notice(const char *conversation,size_t limit,std::string &out) {
    Statement s("SELECT call_id,name,session,arguments,state,error FROM tool_calls WHERE conversation=? AND state IN ('running','unknown') ORDER BY id LIMIT 17");
    s.text(1,conversation);
    std::string content="{\"untrusted\":true,\"warning\":\"Archived unresolved effects, NOT new requests. Never replay these actions automatically.\",\"calls\":[";
    unsigned count=0;int rc;
    while((rc=s.step())==SQLITE_ROW) {
        if(count==16)return error(Error::too_large); // Never silently forget an uncertain effect.
        Json arguments=parse(s.column(3),kTextLimit,512);if(!arguments||!cJSON_IsObject(arguments.get()))return error(Error::invalid);
        if(count++)content+=',';
        content+="{\"call_id\":"+quote(s.column(0))+",\"name\":"+quote(s.column(1))+",\"provider_session\":"+quote(s.column(2))+",\"arguments\":"+s.column(3)+",\"state\":"+quote(s.column(4))+",\"error\":"+quote(s.column(5))+"}";
        if(content.size()>limit)return error(Error::too_large);
    }
    if(rc!=SQLITE_DONE)return sql_error(rc);
    if(count){content+="]}";out="{\"role\":\"assistant\",\"content\":"+quote(content.c_str())+"}";if(out.size()+80>limit)return error(Error::too_large);}
    return {};
}
Result restore(const Recent &q) {
    if(!valid_uuid(q.uuid)||q.limit<256||q.limit>24*1024||!q.max_messages||q.max_messages>48)return error(Error::invalid);
    {Statement exists("SELECT 1 FROM conversations WHERE uuid=?");exists.text(1,q.uuid);int rc=exists.step();if(rc==SQLITE_DONE)return error(Error::not_found);if(rc!=SQLITE_ROW)return sql_error(rc);}
    std::string unresolved;Result notice=unresolved_notice(q.uuid,q.limit,unresolved);if(!notice.ok())return notice;
    const unsigned mandatory=unresolved.empty()?0:1;
    const unsigned available_messages=q.max_messages-mandatory;
    Statement s("SELECT id,session,role,text,interrupted,call_id,tool_name,is_error FROM messages WHERE conversation=? ORDER BY id DESC LIMIT 129");s.text(1,q.uuid);
    std::vector<RestoreRow> group;
    std::vector<std::string> selected;
    size_t selected_bytes=80+(unresolved.empty()?0:unresolved.size()+1),group_bytes=0;
    auto admit=[&]() -> Result {
        std::vector<std::string> complete;
        Result r=complete_group(q.uuid,group,complete);if(!r.ok())return r;
        size_t bytes=0;for(const auto &row:complete)bytes+=row.size()+1;
        if(bytes+selected_bytes>q.limit||complete.size()+selected.size()>available_messages)return error(Error::too_large);
        selected.insert(selected.begin(),std::make_move_iterator(complete.begin()),std::make_move_iterator(complete.end()));
        selected_bytes+=bytes;group.clear();group_bytes=0;return {};
    };
    int rc;
    while((rc=s.step())==SQLITE_ROW) {
        if(cancelled())return error(Error::cancelled);
        RestoreRow row;Result encoded=encode_restore_row(s,q.uuid,row);if(!encoded.ok())return encoded;
        group_bytes+=row.json.size()+1;const bool boundary=row.user;
        group.push_back(std::move(row));
        if(group_bytes+selected_bytes>q.limit||group.size()+selected.size()>available_messages) {
            if(selected.empty())return error(Error::too_large);
            group.clear();break; // Drop the entire older group, never half a tool batch.
        }
        if(boundary) {
            Result r=admit();if(!r.ok()){if(r.error!=Error::too_large||selected.empty())return r;group.clear();break;}
            if(selected.size()==available_messages)break;
        }
    }
    if(rc!=SQLITE_DONE&&rc!=SQLITE_ROW)return sql_error(rc);
    if(!group.empty()){Result r=admit();if(!r.ok()&&(r.error!=Error::too_large||selected.empty()))return r;}
    Result result;result.json="{\"id\":"+quote(q.uuid)+",\"messages\":[";
    for(size_t n=0;n<selected.size();++n){if(n)result.json+=',';result.json+=selected[n];}
    if(!unresolved.empty()){if(!selected.empty())result.json+=',';result.json+=unresolved;}
    result.json+="]}";if(result.json.size()>q.limit)return error(Error::too_large);return result;
}
Result recent(const Recent &q) {
    if(q.limit<296||q.limit>8192)return error(Error::invalid);
    Recent bounded=q;bounded.limit-=40;Result r=restore(bounded);if(!r.ok())return r;
    r.json="{\"untrusted\":true,\"context\":"+r.json+"}";return r;
}
Result erase(const char *id) {
    if(!valid_uuid(id))return error(Error::invalid);
    Transaction tx;
    if(tx.rc!=SQLITE_OK)return sql_error(tx.rc);
    {
        Statement exists("SELECT 1 FROM conversations WHERE uuid=?");
        exists.text(1,id);
        const int rc=exists.step();
        if(rc==SQLITE_DONE)return error(Error::not_found);
        if(rc!=SQLITE_ROW)return sql_error(rc);
    }
    auto failure=[](int code,const char *stage) {
        Result result=sql_error(code);
        result.json="{\"operation\":\"delete_conversation\",\"stage\":"+quote(stage)+"}";
        return result;
    };
    // A single large FK cascade retains conversation-sized row sets and nested
    // trigger work. Explicit child-first batches bound that working set while
    // preserving every FK and ONE atomic commit, including uncertain effects.
    for(const char *table:{"sources","tool_evidence","tool_calls","messages","sessions"}) {
        std::string sql="DELETE FROM ";sql+=table;
        sql+=" WHERE rowid IN (SELECT rowid FROM ";sql+=table;
        sql+=" WHERE conversation=? ORDER BY rowid LIMIT 20)";
        Statement remove(sql.c_str());
        remove.text(1,id);
        for(;;) {
            if(cancelled())return error(Error::cancelled);
            const int rc=remove.step();
            if(rc!=SQLITE_DONE)return failure(rc,table);
            if(sqlite3_changes(db)==0)break;
            remove.rc=sqlite3_reset(remove.p);
            if(remove.rc!=SQLITE_OK)return failure(remove.rc,table);
        }
    }
    Statement remove("DELETE FROM conversations WHERE uuid=?");
    remove.text(1,id);
    int rc=remove.step();
    if(rc!=SQLITE_DONE)return failure(rc,"conversations");
    rc=tx.commit();
    if(rc!=SQLITE_OK)return failure(rc,"commit");
    if(active_id()==id)active("");
    const bool above_limit=db_size()>config.max_database_bytes;
    state(above_limit?State::full:State::ready,above_limit?SQLITE_FULL:0);
    return {};
}
Result source(const Source &v) {
    if(!valid_uuid(v.conversation)||!identity(v.provider_session)||!identity(v.call_id)||!identity(v.source_id)||!bounded(v.provider,160)||!bounded(v.request_id ? v.request_id : "",160)||!bounded(v.url,2048,false)||(strncmp(v.url,"https://",8)&&strncmp(v.url,"http://",7))||!bounded(v.title,256)||!bounded(v.excerpt,1024))return error(Error::invalid);
    for(const unsigned char *p=reinterpret_cast<const unsigned char *>(v.url);*p;++p)if(*p<=32||*p==127)return error(Error::invalid);
    Transaction tx;if(tx.rc!=SQLITE_OK)return sql_error(tx.rc);
    Statement s("INSERT INTO sources(conversation,session,call_id,source_id,provider,request_id,url,title,excerpt,utc_ms,mono_us,boot) SELECT ?,?,?,?,?,?,?,?,?,?,?,? WHERE (SELECT count(*) FROM sources WHERE conversation=? AND session=? AND call_id=?)<4 ON CONFLICT(conversation,session,call_id,url) DO NOTHING");
    s.text(1,v.conversation);s.text(2,v.provider_session);s.text(3,v.call_id);s.text(4,v.source_id);s.text(5,v.provider);s.text(6,v.request_id ? v.request_id : "");s.text(7,v.url);s.text(8,v.title);s.text(9,v.excerpt);bind_time(s,10,v.time);s.text(13,v.conversation);s.text(14,v.provider_session);s.text(15,v.call_id);int rc=s.step();if(rc!=SQLITE_DONE)return sql_error(rc);if(!sqlite3_changes(db)){
        Statement exists("SELECT 1 FROM sources WHERE conversation=? AND session=? AND call_id=? AND url=?");exists.text(1,v.conversation);exists.text(2,v.provider_session);exists.text(3,v.call_id);exists.text(4,v.url);if(exists.step()!=SQLITE_ROW)return error(Error::too_large);
    }rc=tx.commit();return rc==SQLITE_OK?Result{}:sql_error(rc);
}
Outcome outcome(const char *s){if(!strcmp(s,"succeeded"))return Outcome::succeeded;if(!strcmp(s,"failed"))return Outcome::failed;if(!strcmp(s,"cancelled"))return Outcome::cancelled;if(!strcmp(s,"partial"))return Outcome::partial;if(!strcmp(s,"running"))return Outcome::running;return Outcome::unknown;}
Result ledger(Request &request,const ToolIdentity &id,const Finish *finish) {
    if(!valid_uuid(id.conversation)||!identity(id.provider_session)||!identity(id.call_id)||!identity(id.name)||!identity(id.run_id))return error(Error::invalid);
    if(finish&&(!bounded(finish->json,kTextLimit)||finish->outcome==Outcome::running||(finish->error_text&&!bounded(finish->error_text,1024))))return error(Error::invalid);
    Json args=parse(id.arguments_json,kTextLimit,512);if(!args||!cJSON_IsObject(args.get()))return error(Error::invalid);
    std::string canonical_args;if(!canonical(args.get(),canonical_args))return error(Error::invalid);
    const auto digest=hash(canonical_args);if(digest.empty())return error(Error::io);
    Transaction tx;if(tx.rc!=SQLITE_OK)return sql_error(tx.rc);
    Statement prior("SELECT name,argument_hash,state,result,error FROM tool_calls WHERE conversation=? AND session=? AND call_id=?");prior.text(1,id.conversation);prior.text(2,id.provider_session);prior.text(3,id.call_id);int rc=prior.step();
    if(rc!=SQLITE_ROW&&rc!=SQLITE_DONE)return sql_error(rc);
    if(rc==SQLITE_ROW){
        if(strcmp(prior.column(0),id.name)||digest!=prior.column(1)){request.decision=Decision::conflict;return error(Error::conflict);}
        request.outcome=outcome(prior.column(2));request.tool_error=prior.column(4);Result r;r.json=prior.column(3);
        if(!finish){request.decision=request.outcome==Outcome::running?Decision::inflight:request.outcome==Outcome::unknown?Decision::unknown:Decision::reuse_completed;return r;}
        if(request.outcome!=Outcome::running){
            if(request.outcome==finish->outcome&&r.json==finish->json&&(!finish->error_text||request.tool_error==finish->error_text)){request.decision=Decision::reuse_completed;return r;}
            request.decision=request.outcome==Outcome::unknown?Decision::unknown:Decision::conflict;return error(Error::conflict);
        }
        if(finish->outcome==Outcome::running||!bounded(finish->json,kTextLimit))return error(Error::invalid);
        Statement s("UPDATE tool_calls SET state=?,result=?,error=?,finished_utc_ms=?,finished_mono_us=?,finished_boot=? WHERE conversation=? AND session=? AND call_id=? AND state='running'");s.text(1,outcome_name(finish->outcome));s.text(2,finish->json);s.text(3,finish->error_text?finish->error_text:"");bind_time(s,4,id.time);s.text(7,id.conversation);s.text(8,id.provider_session);s.text(9,id.call_id);rc=s.step();if(rc!=SQLITE_DONE)return sql_error(rc);rc=tx.commit();if(rc!=SQLITE_OK){request.outcome=Outcome::unknown;request.decision=Decision::unknown;return sql_error(rc);}request.outcome=finish->outcome;request.decision=Decision::reuse_completed;request.tool_error=finish->error_text?finish->error_text:"";r.json=finish->json;return r;
    }
    if(finish)return error(Error::not_found);
    Result capacity=space();if(!capacity.ok())return capacity;
    std::string redacted;if(!canonical(args.get(),redacted,0,true))return error(Error::invalid);
    Statement s("INSERT INTO tool_calls(conversation,session,call_id,name,argument_hash,arguments,run_id,generation,state,result,utc_ms,mono_us,boot) VALUES(?,?,?,?,?,?,?,?,'running','',?,?,?)");s.text(1,id.conversation);s.text(2,id.provider_session);s.text(3,id.call_id);s.text(4,id.name);s.text(5,digest.c_str());s.text(6,redacted.c_str());s.text(7,id.run_id);s.number(8,id.generation);bind_time(s,9,id.time);rc=s.step();if(rc!=SQLITE_DONE)return sql_error(rc);rc=tx.commit();if(rc!=SQLITE_OK)return sql_error(rc);request.decision=Decision::execute;request.outcome=Outcome::running;return {};
}
Result dispatch(Request &r) {
    if(!db)return error(Error::unavailable);
    if(cancelled())return error(Error::cancelled);
    bool mutating=r.op!=Op::list&&r.op!=Op::history&&r.op!=Op::search&&r.op!=Op::export_rows&&r.op!=Op::recent&&r.op!=Op::restore&&r.op!=Op::begin;
    if(mutating){Result s=space(r.op==Op::erase);if(!s.ok())return s;}
    switch(r.op){
    case Op::create:return create_conversation(static_cast<const char *>(r.data));
    case Op::close:return lifecycle(static_cast<const char *>(r.data),true);
    case Op::resume:return lifecycle(static_cast<const char *>(r.data),false);
    case Op::append:{Transaction tx;if(tx.rc!=SQLITE_OK)return sql_error(tx.rc);Result v=write_message(*static_cast<const Message *>(r.data));if(!v.ok())return v;int rc=tx.commit();return rc==SQLITE_OK?v:sql_error(rc);}
    case Op::boundary:return boundary(*static_cast<const Boundary *>(r.data));
    case Op::checkpoint:return checkpoint(*static_cast<const Checkpoint *>(r.data));
    case Op::list:return list(*static_cast<const List *>(r.data));
    case Op::history:return page(*static_cast<const Query *>(r.data),false);
    case Op::search:return page(*static_cast<const Query *>(r.data),true);
    case Op::export_rows:return export_rows(*static_cast<const Query *>(r.data));
    case Op::recent:return recent(*static_cast<const Recent *>(r.data));
    case Op::restore:return restore(*static_cast<const Recent *>(r.data));
    case Op::erase:return erase(static_cast<const char *>(r.data));
    case Op::source:return source(*static_cast<const Source *>(r.data));
    case Op::begin:return ledger(r,*static_cast<const ToolIdentity *>(r.data),nullptr);
    case Op::finish:{const auto &f=*static_cast<const Finish *>(r.data);return ledger(r,*f.id,&f);}
    }return error(Error::invalid);
}
int initialize() {
#ifndef CONVERSATION_STORE_HOST
    if(config.mount_sd){esp_err_t e=sdcard_mount_checked();if(e!=ESP_OK){state(e==ESP_ERR_NOT_FOUND?State::missing:State::mount_failed,e);return SQLITE_CANTOPEN;}}
#endif
    // Only create the containing application directory, never format/create a filesystem.
    char parent[240];snprintf(parent,sizeof(parent),"%s",path);char *slash=strrchr(parent,'/');if(!slash)return SQLITE_CANTOPEN;*slash=0;
    if(*parent&&mkdir(parent,0700)&&errno!=EEXIST)return SQLITE_CANTOPEN;
#ifdef CONVERSATION_STORE_HOST
    heap=std::malloc(kSqliteHeapBytes);
#else
    heap=heap_caps_malloc(kSqliteHeapBytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#endif
    if(!heap)return SQLITE_NOMEM;
    int rc=sqlite3_config(SQLITE_CONFIG_HEAP,heap,kSqliteHeapBytes,32);if(rc!=SQLITE_OK)return rc;
    rc=sqlite3_config(SQLITE_CONFIG_LOOKASIDE,0,0);if(rc!=SQLITE_OK)return rc;
    rc=internal::configure_vfs(path);if(rc!=SQLITE_OK)return rc;
    rc=sqlite3_initialize();if(rc!=SQLITE_OK)return rc;
    rc=sqlite3_open_v2(path,&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_PRIVATECACHE,"arvel-checked-fat");if(rc!=SQLITE_OK)return rc;
    sqlite3_extended_result_codes(db,1);sqlite3_limit(db,SQLITE_LIMIT_LENGTH,96*1024);sqlite3_limit(db,SQLITE_LIMIT_SQL_LENGTH,8192);sqlite3_limit(db,SQLITE_LIMIT_COLUMN,32);sqlite3_limit(db,SQLITE_LIMIT_EXPR_DEPTH,32);sqlite3_limit(db,SQLITE_LIMIT_ATTACHED,0);sqlite3_limit(db,SQLITE_LIMIT_VARIABLE_NUMBER,32);sqlite3_db_config(db,SQLITE_DBCONFIG_DEFENSIVE,1,nullptr);sqlite3_db_config(db,SQLITE_DBCONFIG_TRUSTED_SCHEMA,0,nullptr);
    rc=exec("PRAGMA page_size=4096; PRAGMA cache_size=-24; PRAGMA temp_store=MEMORY; PRAGMA mmap_size=0; PRAGMA locking_mode=EXCLUSIVE; PRAGMA journal_mode=PERSIST; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON; PRAGMA secure_delete=OFF; PRAGMA journal_size_limit=1048576;");if(rc!=SQLITE_OK)return rc;
    {Statement s("PRAGMA journal_mode");if(s.step()!=SQLITE_ROW||strcmp(s.column(0),"persist"))return SQLITE_ERROR;}
    {Statement s("PRAGMA synchronous");if(s.step()!=SQLITE_ROW||sqlite3_column_int(s.p,0)!=2)return SQLITE_ERROR;}
    {Statement s("PRAGMA foreign_keys");if(s.step()!=SQLITE_ROW||sqlite3_column_int(s.p,0)!=1)return SQLITE_ERROR;}
    char ceiling[96];snprintf(ceiling,sizeof(ceiling),"PRAGMA max_page_count=%llu",static_cast<unsigned long long>(config.max_database_bytes/4096));rc=exec(ceiling);if(rc!=SQLITE_OK)return rc;
    {Statement s("PRAGMA user_version");rc=s.step();if(rc!=SQLITE_ROW)return rc;int version=sqlite3_column_int(s.p,0);if(version!=0&&version!=1)return SQLITE_SCHEMA;
     if(version==0){Statement existing("SELECT count(*) FROM sqlite_schema");if(existing.step()!=SQLITE_ROW||sqlite3_column_int(existing.p,0)!=0)return SQLITE_SCHEMA;}}
    rc=exec("BEGIN IMMEDIATE;"
        "CREATE TABLE IF NOT EXISTS conversations(id INTEGER PRIMARY KEY AUTOINCREMENT,uuid TEXT NOT NULL UNIQUE,title TEXT NOT NULL,closed INTEGER NOT NULL DEFAULT 0,utc_ms INTEGER,mono_us INTEGER NOT NULL,boot TEXT NOT NULL);"
        "CREATE INDEX IF NOT EXISTS conversations_active ON conversations(closed,id);"
        "CREATE TABLE IF NOT EXISTS sessions(id INTEGER PRIMARY KEY AUTOINCREMENT,conversation TEXT NOT NULL REFERENCES conversations(uuid) ON DELETE CASCADE,session TEXT NOT NULL,provider TEXT NOT NULL,model TEXT NOT NULL,reason TEXT NOT NULL,utc_ms INTEGER,mono_us INTEGER NOT NULL,boot TEXT NOT NULL,UNIQUE(conversation,session,reason));"
        "CREATE TABLE IF NOT EXISTS messages(id INTEGER PRIMARY KEY AUTOINCREMENT,conversation TEXT NOT NULL REFERENCES conversations(uuid) ON DELETE CASCADE,session TEXT NOT NULL,item TEXT NOT NULL,role TEXT NOT NULL CHECK(role IN ('user','assistant','tool','tool_unknown')),text TEXT NOT NULL CHECK(length(CAST(text AS BLOB))<=8192),interrupted INTEGER NOT NULL,utc_ms INTEGER,mono_us INTEGER NOT NULL,boot TEXT NOT NULL,call_id TEXT,tool_name TEXT,is_error INTEGER NOT NULL DEFAULT 0,UNIQUE(conversation,session,item));"
        "CREATE INDEX IF NOT EXISTS messages_conversation_id ON messages(conversation,id);"
        "CREATE INDEX IF NOT EXISTS messages_time ON messages(utc_ms,id);"
        "CREATE TABLE IF NOT EXISTS tool_evidence(id INTEGER PRIMARY KEY AUTOINCREMENT,conversation TEXT NOT NULL REFERENCES conversations(uuid) ON DELETE CASCADE,session TEXT NOT NULL,call_id TEXT NOT NULL,name TEXT NOT NULL,arguments TEXT NOT NULL,message_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,UNIQUE(conversation,session,call_id));"
        "CREATE INDEX IF NOT EXISTS tool_evidence_message ON tool_evidence(message_id,id);"
        "CREATE TABLE IF NOT EXISTS tool_calls(id INTEGER PRIMARY KEY AUTOINCREMENT,conversation TEXT NOT NULL REFERENCES conversations(uuid) ON DELETE CASCADE,session TEXT NOT NULL,call_id TEXT NOT NULL,name TEXT NOT NULL,argument_hash TEXT NOT NULL,arguments TEXT NOT NULL,run_id TEXT NOT NULL,generation INTEGER NOT NULL,state TEXT NOT NULL CHECK(state IN ('running','succeeded','failed','unknown','cancelled','partial')),result TEXT NOT NULL,error TEXT NOT NULL DEFAULT '',utc_ms INTEGER,mono_us INTEGER NOT NULL,boot TEXT NOT NULL,finished_utc_ms INTEGER,finished_mono_us INTEGER,finished_boot TEXT,UNIQUE(conversation,session,call_id));"
        "CREATE INDEX IF NOT EXISTS tool_calls_pending ON tool_calls(conversation,state,id);"
        "CREATE TABLE IF NOT EXISTS sources(id INTEGER PRIMARY KEY AUTOINCREMENT,conversation TEXT NOT NULL REFERENCES conversations(uuid) ON DELETE CASCADE,session TEXT NOT NULL,call_id TEXT NOT NULL,source_id TEXT NOT NULL,provider TEXT NOT NULL,request_id TEXT NOT NULL,url TEXT NOT NULL,title TEXT NOT NULL,excerpt TEXT NOT NULL,utc_ms INTEGER,mono_us INTEGER NOT NULL,boot TEXT NOT NULL,UNIQUE(conversation,session,call_id,url),UNIQUE(conversation,session,call_id,source_id));"
        "PRAGMA user_version=1;"
        "UPDATE tool_calls SET state='unknown',error='interrupted_before_durable_terminal_ack',result='{\"ok\":false,\"state\":\"unknown\",\"error\":\"interrupted_before_durable_terminal_ack\"}' WHERE state='running';"
        "COMMIT;");if(rc!=SQLITE_OK)return rc;
    {Statement check("PRAGMA quick_check(1)");rc=check.step();if(rc!=SQLITE_ROW||strcmp(check.column(0),"ok"))return SQLITE_CORRUPT;}
    {
        Statement latest("SELECT uuid FROM conversations WHERE closed=0 ORDER BY id DESC LIMIT 1");
        rc=latest.step();
        if(rc==SQLITE_ROW) {
            if(!valid_uuid(latest.column(0)))return SQLITE_CORRUPT;
            active(latest.column(0));
        } else if(rc!=SQLITE_DONE)return rc;
    }
    sqlite3_progress_handler(db,1000,progress,nullptr);
    return SQLITE_OK;
}
void run_worker() {
    char boot[37]={};const bool have_random=uuid(boot);{std::lock_guard<std::mutex>lock(gate);snprintf(snapshot.boot_uuid,sizeof(snapshot.boot_uuid),"%s",boot);}
    int rc=have_random?initialize():SQLITE_IOERR;
    if(rc==SQLITE_OK){
        Result capacity=reserve_space();const auto bytes=db_size();
        {std::lock_guard<std::mutex>lock(gate);snapshot.sqlite_bytes=sqlite3_memory_used();snapshot.sqlite_highwater=sqlite3_memory_highwater(0);snapshot.database_bytes=bytes;}
        if(capacity.ok())state(State::ready);
    }
    else {State old;{std::lock_guard<std::mutex>lock(gate);old=snapshot.state;}if(old==State::mounting)state((rc&255)==SQLITE_FULL?State::full:State::error,rc);}
    for(;;){
        Request *r;
        {std::unique_lock<std::mutex>lock(gate);work.wait(lock,[]{return queued||stopping;});if(!queued&&stopping)break;r=queue[0];for(size_t i=1;i<queued;++i)queue[i-1]=queue[i];--queued;}
        current=r;r->result=rc==SQLITE_OK?dispatch(*r):error(Error::unavailable,rc);current=nullptr;
        const auto bytes=db_size();const auto used=rc==SQLITE_OK?sqlite3_memory_used():0;const auto high=rc==SQLITE_OK?sqlite3_memory_highwater(0):0;
        {std::lock_guard<std::mutex>lock(gate);snapshot.sqlite_bytes=used;snapshot.sqlite_highwater=high;snapshot.database_bytes=bytes;r->done=true;r->completed.notify_one();}
#ifndef CONVERSATION_STORE_HOST
        vTaskDelay(1); // Back-to-back requests must not starve the idle task between progress yields.
#endif
    }
    if(db){int closed=sqlite3_close(db);if(closed!=SQLITE_OK)state(State::error,closed);db=nullptr;}
    sqlite3_shutdown();std::free(heap);heap=nullptr;
    {std::lock_guard<std::mutex>lock(gate);exited=true;drained.notify_all();}
#ifndef CONVERSATION_STORE_HOST
    vTaskDelete(nullptr);
#endif
}
void submit(Request &r) {
    std::unique_lock<std::mutex>lock(gate);
    if(!started||stopping||snapshot.state==State::mounting||!db){r.result=error(Error::unavailable);return;}
    if(queued==queue.size()){r.result=error(Error::busy);return;}
    if(!r.control.deadline_us)r.control.deadline_us=now_us()+8000000;
    queue[queued++]=&r;work.notify_one();r.completed.wait(lock,[&r]{return r.done;});
}
Result request(Op op,const void *data,Control control){Request r{op,data,control};submit(r);return std::move(r.result);}
} // namespace

const char *outcome_name(Outcome o){switch(o){case Outcome::running:return "running";case Outcome::succeeded:return "succeeded";case Outcome::failed:return "failed";case Outcome::cancelled:return "cancelled";case Outcome::partial:return "partial";default:return "unknown";}}
bool valid_uuid(const char *s){if(!s||strnlen(s,37)!=36)return false;for(unsigned i=0;i<36;++i){if(i==8||i==13||i==18||i==23){if(s[i]!='-')return false;}else if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return false;}return true;}
Error start_async(const Config &c){
    std::lock_guard<std::mutex>lock(gate);if(started)return Error::busy;if(!bounded(c.path,220,false)||c.path[0]!='/'||c.max_database_bytes<128*1024||c.max_database_bytes>1024ULL*1024*1024||c.free_reserve_bytes>1024ULL*1024*1024)return Error::invalid;
    snprintf(path,sizeof(path),"%s",c.path);config=c;config.path=path;snapshot={};snapshot.state=State::mounting;stopping=false;exited=false;started=true;
#ifdef CONVERSATION_STORE_HOST
    worker=std::thread(run_worker);
#else
    // Same priority as the live-voice tasks: callers at priority 5 block on this worker.
    if(xTaskCreate([](void *){run_worker();},"conversation_sd",12288,nullptr,5,&worker)!=pdPASS){started=false;exited=true;snapshot.state=State::error;return Error::unavailable;}
#endif
    return Error::none;
}
Status status(){std::lock_guard<std::mutex>lock(gate);Status s=snapshot;s.queued_requests=queued;return s;}
Result new_conversation(const char *s,Control c){return request(Op::create,s,c);}
Result close_conversation(const char *s,Control c){return request(Op::close,s,c);}
Result resume(const char *s,Control c){return request(Op::resume,s,c);}
Result append_final_message(const Message &m,Control c){return request(Op::append,&m,c);}
Result record_session_boundary(const char *u,const char *s,const char *p,const char *m,const char *r,Time t,Control c){Boundary b{u,s,p,m,r,t};return request(Op::boundary,&b,c);}
Result archive_checkpoint(const char *u,const char *s,const char *j,uint64_t o,Control c,uint64_t from){Checkpoint p{u,s,j,o,from};return request(Op::checkpoint,&p,c);}
Result list_conversations(int64_t cursor,unsigned count,Control c){List q{cursor,count};return request(Op::list,&q,c);}
Result history(const Query &q,Control c){return request(Op::history,&q,c);}
Result search_history(const Query &q,Control c){return request(Op::search,&q,c);}
Result export_page(const Query &q,Control c){return request(Op::export_rows,&q,c);}
Result recent_context(const char *u,size_t limit,Control c){Recent q{u,limit};return request(Op::recent,&q,c);}
Result restore_session(const char *u,size_t limit,unsigned count,Control c){Recent q{u,limit,count};return request(Op::restore,&q,c);}
Result delete_conversation(const char *u,Control c){return request(Op::erase,u,c);}
Result record_source(const Source &s,Control c){return request(Op::source,&s,c);}
ToolResult begin_tool(const ToolIdentity &id,Control c){Request r{Op::begin,&id,c};submit(r);ToolResult out;static_cast<Result &>(out)=std::move(r.result);out.decision=r.decision;out.outcome=r.outcome;out.tool_error=std::move(r.tool_error);return out;}
ToolResult finish_tool(const ToolIdentity &id,Outcome o,const char *json,Control c,const char *error_text){Finish f{&id,o,json,error_text};Request r{Op::finish,&f,c};submit(r);ToolResult out;static_cast<Result &>(out)=std::move(r.result);out.decision=out.ok()?r.decision:Decision::unknown;out.outcome=out.ok()?r.outcome:Outcome::unknown;out.tool_error=std::move(r.tool_error);return out;}
void shutdown(){
    {std::unique_lock<std::mutex>lock(gate);if(!started)return;stopping=true;work.notify_one();drained.wait(lock,[]{return exited;});}
#ifdef CONVERSATION_STORE_HOST
    if(worker.joinable())worker.join();
#endif
    std::lock_guard<std::mutex>lock(gate);started=false;queued=0;snapshot.active_uuid[0]=0;snapshot.state=State::stopped;
}
#ifdef CONVERSATION_STORE_HOST
void inject_fault(Fault f,int after){internal::set_fault(static_cast<int>(f),after);}
#endif
} // namespace conversation_store
