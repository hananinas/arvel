mod model;
mod tool;
mod checkpoint;

use model::{CallbackModel, CallbackWorker};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::ffi::c_void;
use std::io::{self, Write};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::Duration;
use unravel_agent_runtime::{
    AgentLoop, ContentPart, LoopConfig, Message, NoopEventSink, Session, StopToken,
    ToolDefinition, ToolRegistry, ToolExecutionIdentity,
};

const REQUEST_LIMIT: usize = 96 * 1024;
const RESPONSE_LIMIT: usize = 128 * 1024;
const SESSION_LIMIT: usize = 64 * 1024;
const MODEL_RESPONSE_LIMIT: usize = 64 * 1024;
const MODEL_TEXT_LIMIT: usize = 16 * 1024;
const PROMPT_LIMIT: usize = 8 * 1024;
const INVALID_BUFFER: i32 = -1;
const REQUEST_TOO_LARGE: i32 = -2;
const RESPONSE_TOO_SMALL: i32 = -3;
const SETUP_FAILED: i32 = -4;
const BUSY: i32 = -5;
const PANIC: i32 = -6;

pub type ModelCallback = unsafe extern "C" fn(
    *const u8, usize, *mut u8, usize, *mut c_void,
) -> i32;
pub type CheckpointCallback = unsafe extern "C" fn(*const u8, usize, u64, *mut c_void) -> i32;
pub type ClockCallback = unsafe extern "C" fn(*mut c_void) -> u64;

static ACTIVE: AtomicBool = AtomicBool::new(false);
static NEXT_SESSION: AtomicU32 = AtomicU32::new(1);

struct ActiveCall;
impl Drop for ActiveCall {
    fn drop(&mut self) {
        ACTIVE.store(false, Ordering::Release);
    }
}

struct LimitedWriter<W> {
    inner: W,
    count: usize,
    limit: usize,
}
impl<W: Write> Write for LimitedWriter<W> {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        if bytes.len() > self.limit.saturating_sub(self.count) {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "JSON byte limit exceeded"));
        }
        let written = self.inner.write(bytes)?;
        self.count += written;
        Ok(written)
    }
    fn flush(&mut self) -> io::Result<()> {
        self.inner.flush()
    }
}

fn bounded_json(value: &impl Serialize, limit: usize) -> serde_json::Result<Vec<u8>> {
    let mut writer = LimitedWriter { inner: Vec::with_capacity(limit.min(4096)), count: 0, limit };
    serde_json::to_writer(&mut writer, value)?;
    Ok(writer.inner)
}

fn encoded_len(value: &impl Serialize, limit: usize) -> serde_json::Result<usize> {
    let mut writer = LimitedWriter { inner: io::sink(), count: 0, limit };
    serde_json::to_writer(&mut writer, value)?;
    Ok(writer.count)
}

fn fits(value: &impl Serialize, limit: usize) -> bool {
    encoded_len(value, limit).is_ok()
}

fn fresh_session() -> Session {
    Session::new(format!("esp32-{}", NEXT_SESSION.fetch_add(1, Ordering::Relaxed)))
}

fn error(id: Value, code: i32, message: &str) -> Value {
    json!({"jsonrpc":"2.0","id":id,"error":{"code":code,"message":message}})
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct RunParams {
    prompt: String,
    model: String,
    #[serde(default)]
    session: Option<Session>,
    #[serde(default)]
    system_prompt: Option<String>,
    #[serde(default)]
    tools: Vec<ToolDefinition>,
    #[serde(default)]
    message_offset: u64,
    #[serde(default)]
    execution: ToolExecutionIdentity,
}

fn valid_session(session: &Session) -> bool {
    !session.id.trim().is_empty()
        && session.id.len() <= 128
        && !session.id.contains('\0')
        && session.messages.len() <= 128
        && fits(session, SESSION_LIMIT)
        && session.messages.iter().all(|message| match message {
            Message::User { content } => content.parts.iter()
                .all(|part| matches!(part, ContentPart::Text { text } if !text.contains('\0'))),
            Message::Assistant { content, tool_calls, reasoning } => !content.contains('\0')
                && reasoning.as_ref().and_then(|value| value.text.as_ref()).is_none_or(|text| !text.contains('\0'))
                && tool_calls.len() <= 8
                && tool_calls.iter().all(|call| !call.id.is_empty() && call.id.len() <= 128 &&
                    !call.id.contains('\0') && !call.name.is_empty() && call.name.len() <= 64 &&
                    !call.name.contains('\0') && call.arguments.is_object() && fits(&call.arguments, 8192)),
            Message::Tool { call_id, name, content, .. } =>
                call_id.len() <= 128 && name.len() <= 64 && !call_id.contains('\0') &&
                !name.contains('\0') && !content.contains('\0'),
            Message::ToolUnknown { call_id, name, reason } =>
                call_id.len() <= 128 && name.len() <= 64 && !call_id.contains('\0') &&
                !name.contains('\0') && !reason.contains('\0'),
            Message::System { content } => !content.contains('\0'),
        })
}

fn run(id: Value, params: Value, callback: Option<ModelCallback>,
    tool_callback: Option<ModelCallback>, checkpoint_callback: Option<CheckpointCallback>,
    clock_callback: Option<ClockCallback>, context: usize)
    -> std::result::Result<Value, i32>
{
    let params: RunParams = match serde_json::from_value(params) {
        Ok(params) => params,
        Err(_) => return Ok(error(id, -32602, "agent.run requires prompt, model, and optional session/system_prompt")),
    };
    if params.prompt.trim().is_empty() || params.prompt.len() > PROMPT_LIMIT
        || params.model.trim().is_empty() || params.model.len() > 256
        || params.system_prompt.as_ref().is_some_and(|text| text.len() > PROMPT_LIMIT)
    {
        return Ok(error(id, -32602, "prompt, model, or system_prompt is empty or exceeds its byte limit"));
    }
    let mut session = params.session.unwrap_or_else(fresh_session);
    let mut message_offset = params.message_offset;
    let mut execution = params.execution;
    if execution.run_id.is_empty() {
        execution.run_id = format!("esp-run-{}", NEXT_SESSION.fetch_add(1, Ordering::Relaxed));
    }
    if [execution.conversation_id.as_str(), execution.provider_session.as_str(), execution.run_id.as_str()]
        .iter().any(|id| id.len() > 128 || id.contains('\0')) ||
        message_offset > checkpoint::MAX_OFFSET || execution.origin.len() > 32 {
        return Ok(error(id, -32602, "execution identity or offset exceeds limits"));
    }
    if !valid_session(&session) {
        return Ok(error(id, -32602, "session exceeds limits or contains unsupported content"));
    }
    if params.tools.len() > 16 || params.tools.iter().any(|tool| {
        tool.name.trim().is_empty() || tool.name.len() > 64
            || tool.description.len() > 2048 || !tool.parameters.is_object()
            || !fits(&tool.parameters, 8 * 1024)
    }) {
        return Ok(error(id, -32602, "tool definitions exceed limits or lack a JSON schema object"));
    }
    let mut tools = ToolRegistry::new();
    let history_budget = Arc::new(AtomicUsize::new(0));
    let worker = Arc::new(CallbackWorker::default());
    if !params.tools.is_empty() {
        let tool_callback = tool_callback.ok_or(SETUP_FAILED)?;
        let clock_callback = clock_callback.ok_or(SETUP_FAILED)?;
        for definition in params.tools {
            if let Err(failure) = tools.register(tool::CallbackTool {
                definition, callback:tool_callback, clock:clock_callback, context, worker:worker.clone(),
                history_budget:history_budget.clone(),
            }) {
                return Ok(error(id, -32602, &failure.to_string()));
            }
        }
    }
    let callback = callback.ok_or(SETUP_FAILED)?;
    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_time()
        .build().map_err(|_| SETUP_FAILED)?;
    let model = Arc::new(CallbackModel {
        model:params.model, callback, context, worker:worker.clone(), history_budget,
    });
    let config = LoopConfig {
        max_turns: 4,
        // A blocking C callback cannot be forcibly cancelled. Do not queue
        // retries behind an in-flight timed-out call on a memory-limited device.
        max_model_attempts: 1,
        model_deadline: Some(Duration::from_secs(45)),
        tool_deadline: Some(Duration::from_secs(10)),
        max_tool_calls_per_response: 8,
        max_output_bytes: MODEL_TEXT_LIMIT,
        max_observation_bytes: 0,
        max_output: Some(1024),
        prefer_streaming: false,
        max_parallel_tools: 1,
        ..LoopConfig::default()
    };
    let agent = AgentLoop::new(model, tools,
        params.system_prompt.unwrap_or_else(|| "You are a helpful assistant.".into()))
        .with_config(config)
        .with_execution_identity(execution);
    let mut checkpoint = |session: &mut Session| {
        let archive = checkpoint_callback.map(|callback| move |bytes: &[u8], offset| {
            // Synchronous acknowledgment on the non-audio RPC worker.
            if unsafe { callback(bytes.as_ptr(), bytes.len(), offset, context as *mut c_void) } != 1 {
                return Err(unravel_agent_runtime::Error::Tool("archive acknowledgment failed; history retained".into()));
            }
            Ok(())
        });
        checkpoint::checkpoint(session, &mut message_offset, archive)
    };
    let result = runtime.block_on(agent.run_with_checkpoint(&mut session, params.prompt,
        &NoopEventSink, &StopToken::new(), &mut checkpoint));
    // Stop async work, then join any callback whose wait timed out. Native
    // callbacks cannot be preempted, and their context is caller-owned.
    drop(runtime);
    worker.join().map_err(|_| SETUP_FAILED)?;
    let reply = match result {
        Ok(output) if valid_session(&session) => {
            json!({"jsonrpc":"2.0","id":id,"result":{"output":output,"session":session,"message_offset":message_offset}})
        }
        Ok(_) => json!({"jsonrpc":"2.0","id":id,"error":{
            "code":-32001,"message":"updated session exceeds limits; reset or explicitly edit history",
            "data":{"session":session,"message_offset":message_offset}
        }}),
        Err(failure) => json!({"jsonrpc":"2.0","id":id,"error":{
            "code":-32000,"message":failure.to_string(),"data":{"session":session,"message_offset":message_offset}
        }}),
    };
    Ok(reply)
}

fn dispatch(bytes: &[u8], callback: Option<ModelCallback>,
    tool_callback: Option<ModelCallback>, checkpoint_callback: Option<CheckpointCallback>,
    clock_callback: Option<ClockCallback>, context: usize)
    -> std::result::Result<Option<Value>, i32>
{
    let request: Value = match serde_json::from_slice(bytes) {
        Ok(request) => request,
        Err(_) => return Ok(Some(error(Value::Null, -32700, "Parse error"))),
    };
    let mut object = match request {
        Value::Object(object) => object,
        _ => return Ok(Some(error(Value::Null, -32600, "Invalid Request"))),
    };
    let id = object.remove("id");
    let valid_id = id.as_ref().is_none_or(|id| matches!(id, Value::Null | Value::String(_) | Value::Number(_)));
    if object.get("jsonrpc").and_then(Value::as_str) != Some("2.0")
        || !object.get("method").is_some_and(Value::is_string) || !valid_id
        || object.get("params").is_some_and(|params| !params.is_object() && !params.is_array())
    {
        let id = if valid_id { id.unwrap_or(Value::Null) } else { Value::Null };
        return Ok(Some(error(id, -32600, "Invalid Request")));
    }
    let notification = id.is_none();
    let id = id.unwrap_or(Value::Null);
    let method = object.remove("method").unwrap_or(Value::Null);
    let params = object.remove("params");
    let reply = match method.as_str() {
        Some("agent.reset") => {
            if params.as_ref().is_some_and(|params| !params.as_object().is_some_and(|object| object.is_empty())) {
                error(id, -32602, "agent.reset accepts only absent params or an empty object")
            } else {
                json!({"jsonrpc":"2.0","id":id,"result":{"session":fresh_session()}})
            }
        }
        Some("agent.run") => run(id, params.unwrap_or(Value::Null), callback, tool_callback, checkpoint_callback, clock_callback, context)?,
        Some("agent.session") => {
            let value = params.and_then(|mut p| p.as_object_mut().and_then(|p| p.remove("session")));
            match value.and_then(|v| serde_json::from_value::<Session>(v).ok()) {
                Some(session) if valid_session(&session) && checkpoint::valid_groups(&session.messages)
                    && session.messages.len() <= checkpoint::CONTEXT_MESSAGES
                    && fits(&session, checkpoint::CONTEXT_BYTES) =>
                    json!({"jsonrpc":"2.0","id":id,"result":{"session":session}}),
                _ => error(id, -32602, "invalid bounded session or malformed complete tool groups"),
            }
        }
        _ => error(id, -32601, "Method not found"),
    };
    Ok(if notification { None } else { Some(reply) })
}

fn valid_range(pointer: usize, length: usize) -> bool {
    pointer != 0 && length <= isize::MAX as usize && pointer.checked_add(length).is_some()
}

/// See agent_runtime.h for pointer ownership, limits and callback requirements.
///
/// # Safety
/// Native callers must supply live, non-overlapping readable/writable buffers
/// of the stated sizes and a callback/context valid until this function returns.
#[no_mangle]
pub unsafe extern "C" fn unravel_agent_rpc(
    request: *const u8, request_len: usize,
    response: *mut u8, response_capacity: usize,
    callback: Option<ModelCallback>, tool_callback: Option<ModelCallback>,
    checkpoint_callback: Option<CheckpointCallback>, clock_callback: Option<ClockCallback>, context: *mut c_void,
) -> i32 {
    if !valid_range(request as usize, request_len)
        || !valid_range(response as usize, response_capacity)
        || response_capacity == 0
    {
        return INVALID_BUFFER;
    }
    if request_len > REQUEST_LIMIT {
        return REQUEST_TOO_LARGE;
    }
    let request_start = request as usize;
    let response_start = response as usize;
    if request_start < response_start + response_capacity
        && response_start < request_start + request_len
    {
        return INVALID_BUFFER;
    }
    if ACTIVE.compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed).is_err() {
        return BUSY;
    }
    let _active = ActiveCall;
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        // SAFETY: ranges checked above; validity of native allocations is the
        // caller's responsibility. No callback borrows either ABI buffer.
        let request = unsafe { std::slice::from_raw_parts(request, request_len) };
        let reply = match dispatch(request, callback, tool_callback, checkpoint_callback, clock_callback, context as usize) {
            Ok(Some(reply)) => reply,
            Ok(None) => return 0,
            Err(code) => return code,
        };
        let capacity = response_capacity.min(RESPONSE_LIMIT);
        let output = unsafe { std::slice::from_raw_parts_mut(response, capacity) };
        let mut writer = LimitedWriter { inner:output, count:0, limit:capacity };
        if serde_json::to_writer(&mut writer, &reply).is_err() {
            return RESPONSE_TOO_SMALL;
        }
        writer.count as i32
    }));
    result.unwrap_or(PANIC)
}
