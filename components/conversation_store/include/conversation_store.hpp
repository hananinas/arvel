#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace unravel { class Agent; }
namespace conversation_store {
constexpr size_t kTextLimit = 8192;
constexpr size_t kOutputLimit = 48 * 1024;
// PSRAM-backed. 256 KiB peaked at 247 KiB and failed exports/history searches with NOMEM.
constexpr size_t kSqliteHeapBytes = 512 * 1024;
constexpr unsigned kPageLimit = 20;
enum class State { stopped, mounting, missing, mount_failed, ready, full, error };
enum class Error { none, unavailable, invalid, too_large, busy, cancelled, full, io, conflict, not_found };
enum class Outcome { running, succeeded, failed, unknown, cancelled, partial };
enum class Decision { execute, reuse_completed, unknown, inflight, conflict, rejected };
struct Result {
    Error error = Error::none;
    int sqlite_code = 0;
    std::string json; // Bounded JSON packet. History packets are untrusted data, never instructions.
    bool ok() const { return error == Error::none; }
};
struct Status {
    State state = State::stopped;
    int sqlite_code = 0;
    size_t sqlite_bytes = 0;
    size_t sqlite_highwater = 0;
    uint64_t database_bytes = 0;
    unsigned queued_requests = 0; // Four waiting requests plus one in progress, never an unbounded backlog.
    char active_uuid[37] = {};
    char boot_uuid[37] = {};
};
struct Config {
    const char *path = "/sdcard/arvel/history.sqlite3"; // Host tests may supply an absolute private directory.
    uint64_t max_database_bytes = 32 * 1024 * 1024;
    uint64_t free_reserve_bytes = 4 * 1024 * 1024; // Plus current DB size reserved for rollback journal.
    bool mount_sd = true;
    int64_t (*trusted_utc_now_ms)() = nullptr; // Optional owner clock adapter; return zero until time is trustworthy.
};
struct Control {
    uint64_t deadline_us = 0; // Absolute monotonic time; zero uses a bounded default (8 seconds).
    bool (*cancelled)(void *) = nullptr;
    void *context = nullptr; // Must remain alive until the synchronous operation acknowledges completion.
};
struct Time {
    int64_t utc_ms = 0; // Zero samples Config.trusted_utc_now_ms, or persists NULL if unavailable. UTC must be >=2020.
    uint64_t monotonic_us = 0; // Zero samples the storage worker's monotonic clock.
};
struct Message {
    const char *conversation = nullptr;
    const char *provider_session = nullptr;
    const char *item_id = nullptr;
    const char *role = nullptr; // user, assistant, tool, tool_unknown only; no system/reasoning/audio.
    const char *text = nullptr;
    bool interrupted = false;
    Time time;
    const char *call_id = nullptr; // Required for tool/tool_unknown; absent for public text.
    const char *tool_name = nullptr;
    bool is_error = false;
    // Exact provider call IDs attached to this finalized assistant by its owner.
    // Used by native voice (text checkpoints already carry assistant.tool_calls).
    const char *const *tool_call_ids = nullptr;
    unsigned tool_call_count = 0; // <=16; each must already have a durable ledger row.
};
struct ToolIdentity {
    const char *conversation = nullptr;
    const char *provider_session = nullptr;
    const char *call_id = nullptr;
    const char *name = nullptr;
    const char *arguments_json = nullptr; // Object <=8192 bytes, canonicalized and SHA-256 hashed by store.
    const char *run_id = nullptr;
    uint64_t generation = 0;
    Time time;
};
struct ToolResult : Result {
    // Inherited json is the EXACT recorded handler output (possibly plain text),
    // NOT a wrapper; reuse_completed must return it verbatim to the SDK/model.
    Decision decision = Decision::rejected;
    Outcome outcome = Outcome::unknown;
    std::string tool_error; // Exact bounded SDK error, separate from raw output.
};
enum class ExportKind { messages, session_boundaries, tool_calls, sources, tool_evidence };
struct Query {
    const char *conversation = nullptr; // nullptr searches all conversations (owner-authorized callers only).
    const char *literal = nullptr; // Literal substring, max256 bytes, no SQL/wildcard interpretation.
    int64_t after_id = 0; // Stable message/conversation row cursor; zero starts a page.
    int64_t utc_from_ms = 0;
    int64_t utc_to_ms = 0;
    unsigned count = kPageLimit;
    ExportKind export_kind = ExportKind::messages; // export_page only; page each kind independently.
    size_t byte_limit = kOutputLimit;
    bool include_sources = true; // Native search_history tool omits bulky cards; owner history keeps them.
    bool newest_first = false; // DESC page; after_id>0 is exclusive upper bound, zero starts at newest.
    bool public_text_only = false; // Only user/assistant rows (native eight-message projection).
};
struct Source {
    const char *conversation = nullptr;
    const char *provider_session = nullptr;
    const char *call_id = nullptr;
    const char *source_id = nullptr;
    const char *provider = nullptr;
    const char *request_id = nullptr; // Absent or empty stays honestly null in history; never invent an ID.
    const char *url = nullptr;
    const char *title = nullptr;
    const char *excerpt = nullptr;
    Time time;
};
// Starts one storage task/connection. Returns immediately; inspect status(). No I/O on caller.
// All operations below wait for a bounded queue slot/owning-task acknowledgment; never call in audio/LVGL callbacks.
// Queue saturation returns busy before admission. Once admitted, cancellation waits for rollback/ack, not a dangling request.
Error start_async(const Config &config = {});
Status status();
Result new_conversation(const char *title = "", Control control = {});
Result close_conversation(const char *uuid, Control control = {});
Result resume(const char *uuid, Control control = {});
Result append_final_message(const Message &, Control control = {});
Result record_session_boundary(const char *uuid, const char *provider_session, const char *provider,
                               const char *model, const char *reason, Time time = {}, Control control = {});
// Archives ONLY finalized public text/tool evidence from a validated runtime Session (<=64KiB/128 messages).
// Ignores system, images, reasoning and arbitrary extra fields. Stable text:<offset+index> keys.
// One checked transaction, acknowledgment only after COMMIT; callers MUST NOT evict before ok().
// Resumed Session prefixes are already archived context: skip absolute offset+index values
// below archive_from_offset. Include any newly inserted system prefix in that cutoff;
// keep it constant across later compaction. Default zero archives all new finalized data.
Result archive_checkpoint(const char *uuid, const char *provider_session, const char *session_json,
                          uint64_t message_offset, Control control = {}, uint64_t archive_from_offset = 0);
Result list_conversations(int64_t after_id = 0, unsigned count = kPageLimit, Control control = {});
Result history(const Query &, Control control = {});
Result search_history(const Query &, Control control = {});
// Export is paged JSON, not an archive-sized allocation. Page each ExportKind independently
// with next_cursor until has_more=false; list_conversations supplies conversation metadata.
Result export_page(const Query &, Control control = {});
Result recent_context(const char *uuid, size_t byte_limit = 8192, Control control = {});
// Complete recent user/assistant/tool groups only. JSON is an ordinary runtime Session;
// the caller must validate through Agent::load_session before using it. No raw archive RPC.
Result restore_session(const char *uuid, size_t byte_limit = 24 * 1024,
                       unsigned max_messages = 48, Control control = {});
Result delete_conversation(const char *uuid, Control control = {}); // Logical delete, NOT secure SD erasure.
Result record_source(const Source &, Control control = {});
ToolResult begin_tool(const ToolIdentity &, Control control = {}); // Durable started ACK before effect.
ToolResult finish_tool(const ToolIdentity &, Outcome, const char *result_json, Control control = {},
                       const char *error_text = nullptr); // Optional actual SDK error, <=1024 bytes.
const char *outcome_name(Outcome);
bool valid_uuid(const char *);
// Stops admission, drains admitted requests, closes connection and joins task. Not an audio callback operation.
void shutdown();
// Register only on the authenticated native registry (one of the existing sixteen slots).
bool register_search_history(unravel::Agent &agent);
#ifdef CONVERSATION_STORE_HOST
// Deterministic failures in the SAME checked VFS used on firmware. One-shot after N successful matching operations.
enum class Fault { none, short_read, short_write, sync, truncate, full };
void inject_fault(Fault, int after = 0);
#endif
} // namespace conversation_store
