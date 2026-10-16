#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UNRAVEL_RPC_MAX_REQUEST_BYTES (96u * 1024u)
#define UNRAVEL_RPC_MAX_RESPONSE_BYTES (128u * 1024u)
#define UNRAVEL_SESSION_MAX_BYTES (64u * 1024u)
#define UNRAVEL_MODEL_MAX_RESPONSE_BYTES (64u * 1024u)
#define UNRAVEL_PROMPT_MAX_BYTES (8u * 1024u)
#define UNRAVEL_MODEL_MAX_OUTPUT_TOKENS 1024u
#define UNRAVEL_TOOL_MAX_RESPONSE_BYTES (32u * 1024u)
#define UNRAVEL_MAX_TOOLS 16u
#define UNRAVEL_CONTEXT_MAX_BYTES (24u * 1024u)
#define UNRAVEL_CONTEXT_MAX_MESSAGES 48u

/* Negative ABI errors; JSON-RPC/application errors are nonnegative JSON replies. */
#define UNRAVEL_RPC_INVALID_BUFFER (-1)
#define UNRAVEL_RPC_REQUEST_TOO_LARGE (-2)
#define UNRAVEL_RPC_RESPONSE_TOO_SMALL (-3)
#define UNRAVEL_RPC_SETUP_FAILED (-4)
#define UNRAVEL_RPC_BUSY (-5)
#define UNRAVEL_RPC_PANIC (-6)

/*
 * Receives non-streaming OpenAI Chat Completions request JSON (no credentials)
 * and returns response JSON bytes, or a negative transport error. Provider
 * error bodies may be returned normally, including {"error":{...}}; optional
 * top-level "status" carries HTTP status for retry classification.
 * Runs on a fallibly created pthread with a 12 KiB internal stack, NOT the caller's task.
 * Worker allocation failure returns an agent error rather than aborting.
 * Neither buffer is NUL-terminated. Both belong to Rust and are valid only
 * during this callback. Do not retain either pointer or exceed capacity.
 * context may be NULL. The callback and its context must remain valid until
 * unravel_agent_rpc returns. Configure a bounded HTTPS timeout: Rust deadlines
 * cancel the async wait, but cannot preempt C/C++ code. RPC waits for any
 * in-flight callback to finish before returning, even after a deadline, so
 * caller-owned context can never be accessed after return.
 */
typedef int32_t (*unravel_model_callback_t)(
    const uint8_t *request, size_t request_len,
    uint8_t *response, size_t response_capacity, void *context);

/*
 * Optional tool executor. Receives {name,arguments,call_id,run_id,attempt,
 * deadline_us,generation,conversation_id,provider_session,origin}
 * and returns {content:string,state:string,metadata?:JSON}.
 * Uses the same lifetime, worker-stack and buffer-ownership rules as the model
 * callback. Tools execute serially through the real upstream ToolRegistry;
 * execution failures become upstream error tool results. Tool callbacks must
 * also bound their own work: the 10-second async deadline cannot preempt C++.
 */
typedef int32_t (*unravel_tool_callback_t)(
    const uint8_t *request, size_t request_len,
    uint8_t *response, size_t response_capacity, void *context);

/* Synchronous archive acknowledgment before history eviction; 1 acknowledges,
 * zero/negative stops admission without eviction. Called on the RPC worker,
 * never on an audio callback. Session is bounded canonical JSON; item identity
 * is message_offset+index (leading system messages are not archived). */
typedef int32_t (*unravel_checkpoint_callback_t)(
    const uint8_t *session, size_t length, uint64_t message_offset, void *context);
/* Required with tools: returns the caller's monotonic microsecond clock.
 * Allows absolute deadlines to survive callback worker scheduling delays. */
typedef uint64_t (*unravel_clock_callback_t)(void *context);

/*
 * Synchronous JSON-RPC 2.0 single-request entry point (no batches).
 * agent.run params: {prompt,model,session?,system_prompt?,tools?,
 *                    message_offset?,execution?:{conversation_id,provider_session,
 *                    run_id,generation,origin}}
 * Result: {output:string,session: canonical upstream Session,message_offset}.
 * Runtime errors include error.data.session with reconciled history.
 * agent.reset accepts absent params or {} and returns {session: fresh Session}.
 * Valid notifications execute and return 0; IDs (string/number/null) are retained.
 * Working history is compacted by complete groups to 24 KiB/48 messages.
 * A configured checkpoint must acknowledge before eviction; absent a callback,
 * bounded ephemeral compaction is explicit. Hard limits remain 64 KiB/128.
 * agent.session validates canonical Session without model/tool execution.
 * Returns the written byte count without NUL termination, or an ABI error above.
 * Only one invocation can be active (including callback) at a time; recursive
 * calls and concurrent calls return UNRAVEL_RPC_BUSY without waiting.
 * request/response must point to readable/writable allocations of their stated
 * lengths, which must not overlap. NULL/overflowing ranges are rejected; this
 * ABI cannot detect arbitrary invalid non-NULL pointers from native callers.
 * response_capacity may exceed MAX_RESPONSE; Rust never writes above that cap.
 * No callback is required for reset or protocol errors; agent.run requires a
 * model callback and, when tools are supplied, a tool callback.
 * Caller must provide a task stack suitable for Rust JSON parsing (64 KiB
 * recommended) and heap capacity for bounded sessions/provider JSON.
 */
int32_t unravel_agent_rpc(
    const uint8_t *request, size_t request_len,
    uint8_t *response, size_t response_capacity,
    unravel_model_callback_t callback, unravel_tool_callback_t tool_callback,
    unravel_checkpoint_callback_t checkpoint_callback,
    unravel_clock_callback_t clock_callback, void *context);

#ifdef __cplusplus
}
#endif
