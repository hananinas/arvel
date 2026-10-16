use crate::{bounded_json, encoded_len, ModelCallback, MODEL_RESPONSE_LIMIT, MODEL_TEXT_LIMIT, REQUEST_LIMIT, SESSION_LIMIT};
use async_trait::async_trait;
use serde_json::{json, Value};
use std::ffi::c_void;
use std::sync::{Arc, Mutex, atomic::{AtomicUsize, Ordering}};
use std::thread::{self, JoinHandle};
use tokio::sync::oneshot;
use unravel_agent_runtime::{
    ContentPart, FinishReason, Message, Model, ModelError, ModelRequest, ModelResponse,
    Reasoning, Result, ToolCall, Usage,
};

pub(crate) struct CallbackModel {
    pub model: String,
    pub callback: ModelCallback,
    // The synchronous RPC joins callback workers before releasing this context.
    // Store the address, not a borrowed buffer or Rust reference.
    pub context: usize,
    pub worker: Arc<CallbackWorker>,
    pub history_budget: Arc<AtomicUsize>,
}

fn invalid(message: impl Into<String>) -> unravel_agent_runtime::Error {
    ModelError::invalid(message).into()
}

fn payload(model: &str, request: ModelRequest) -> Result<Vec<u8>> {
    let mut messages = Vec::with_capacity(request.messages.len());
    for message in request.messages {
        let wire = match message {
            Message::System { content } => json!({"role":"system","content":content}),
            Message::User { content } => {
                if content.parts.iter().any(|part| !matches!(part, ContentPart::Text { .. })) {
                    return Err(invalid("image history is unsupported by the embedded text adapter"));
                }
                json!({"role":"user","content":content.to_text()})
            }
            Message::Assistant { content, tool_calls, reasoning } => {
                let mut wire = json!({
                    "role":"assistant",
                    "content":if content.is_empty() { Value::Null } else { Value::String(content) }
                });
                if !tool_calls.is_empty() {
                    let calls: std::result::Result<Vec<_>, _> = tool_calls.into_iter().map(|call| {
                        Ok(json!({"id":call.id,"type":"function","function":{
                            "name":call.name,
                            "arguments":serde_json::to_string(&call.arguments)?
                        }}))
                    }).collect::<std::result::Result<_, serde_json::Error>>();
                    wire["tool_calls"] = Value::Array(calls.map_err(|e| invalid(e.to_string()))?);
                }
                if let Some(Reasoning { text: Some(text) }) = reasoning {
                    if !text.is_empty() {
                        wire["reasoning_content"] = Value::String(text);
                    }
                }
                wire
            }
            Message::Tool { call_id, name, content, is_error } => {
                // OpenAI requires a string, not a JSON object, in tool.content.
                let content = if is_error {
                    serde_json::to_string(&json!({"content":content,"is_error":true}))
                        .map_err(|e| invalid(e.to_string()))?
                } else { content };
                json!({"role":"tool","tool_call_id":call_id,"name":name,"content":content})
            }
            Message::ToolUnknown { .. } => {
                return Err(invalid("unresolved tool outcome cannot be sent to a model"));
            }
        };
        messages.push(wire);
    }
    let mut body = json!({"model":model,"messages":messages,"stream":false});
    if !request.tools.is_empty() {
        body["tools"] = Value::Array(request.tools.into_iter().map(|tool| {
            json!({"type":"function","function":{
                "name":tool.name,"description":tool.description,"parameters":tool.parameters
            }})
        }).collect());
    }
    if let Some(max_output) = request.max_output {
        body["max_tokens"] = json!(max_output);
    }
    if let Some(sampling) = request.sampling {
        if let Some(temperature) = sampling.temperature {
            body["temperature"] = json!(temperature);
        }
        if let Some(top_p) = sampling.top_p {
            body["top_p"] = json!(top_p);
        }
        if let Some(max_tokens) = sampling.max_tokens {
            body["max_tokens"] = json!(max_tokens.min(1024));
        }
    }
    bounded_json(&body, REQUEST_LIMIT).map_err(|_| invalid("provider request exceeds byte limit"))
}

fn provider_error(body: &Value) -> Option<unravel_agent_runtime::Error> {
    let error = body.get("error").filter(|value| !value.is_null())?;
    let message = error.get("message").and_then(Value::as_str)
        .unwrap_or("provider returned an error");
    let message = if message.len() <= 1024 { message }
        else { "provider error message exceeds byte limit" };
    let code = error.get("code").and_then(Value::as_str).unwrap_or("");
    let kind = error.get("type").and_then(Value::as_str).unwrap_or("");
    let status = body.get("status").and_then(Value::as_u64);
    let typed = if matches!(status, Some(401 | 403))
        || matches!(code, "invalid_api_key" | "authentication_error") {
        ModelError::auth(message)
    } else if matches!(code, "insufficient_quota" | "billing_hard_limit_reached") {
        ModelError::invalid(message)
    } else if matches!(status, Some(408 | 429 | 500..=599))
        || matches!(code, "rate_limit_exceeded" | "server_error" | "timeout")
        || matches!(kind, "rate_limit_error" | "server_error") {
        ModelError::transient(message)
    } else {
        ModelError::invalid(message)
    };
    Some(typed.into())
}

fn parse_response(bytes: &[u8]) -> Result<ModelResponse> {
    let body: Value = serde_json::from_slice(bytes)
        .map_err(|_| invalid("provider response is not valid UTF-8 JSON"))?;
    if let Some(error) = provider_error(&body) {
        return Err(error);
    }
    let choices = body.get("choices").and_then(Value::as_array)
        .ok_or_else(|| invalid("provider response has no choices array"))?;
    if choices.len() != 1 {
        return Err(invalid("provider must return exactly one choice"));
    }
    let choice = &choices[0];
    let message = choice.get("message").and_then(Value::as_object)
        .ok_or_else(|| invalid("provider choice has no message object"))?;
    if message.get("role").and_then(Value::as_str) != Some("assistant") {
        return Err(invalid("provider message role must be assistant"));
    }
    let content = match message.get("content") {
        None | Some(Value::Null) => String::new(),
        Some(Value::String(text)) if text.len() <= MODEL_TEXT_LIMIT && !text.contains('\0') => text.clone(),
        Some(Value::String(_)) => return Err(invalid("assistant text exceeds byte limit or contains NUL")),
        _ => return Err(invalid("assistant content must be text or null")),
    };
    if message.get("refusal").is_some_and(|value| !value.is_null()) {
        return Err(invalid("provider refused the request"));
    }
    let mut tool_calls = Vec::new();
    if let Some(raw_calls) = message.get("tool_calls") {
        let calls = raw_calls.as_array().ok_or_else(|| invalid("tool_calls must be an array"))?;
        if calls.len() > 8 {
            return Err(invalid("provider tool call count exceeds limit"));
        }
        for call in calls {
            if call.get("type").and_then(Value::as_str) != Some("function") {
                return Err(invalid("only complete function tool calls are supported"));
            }
            let id = call.get("id").and_then(Value::as_str)
                .filter(|value| !value.trim().is_empty())
                .ok_or_else(|| invalid("tool call has no nonempty string id"))?;
            if id.len() > 128 || id.contains('\0') {
                return Err(invalid("tool call id exceeds byte limit or contains NUL"));
            }
            let function = call.get("function").and_then(Value::as_object)
                .ok_or_else(|| invalid("tool call has no function object"))?;
            let name = function.get("name").and_then(Value::as_str)
                .filter(|value| !value.trim().is_empty())
                .ok_or_else(|| invalid("tool call has no nonempty function name"))?;
            if name.len() > 64 || name.contains('\0') {
                return Err(invalid("tool function name exceeds byte limit or contains NUL"));
            }
            let arguments = function.get("arguments").and_then(Value::as_str)
                .ok_or_else(|| invalid("tool arguments must be a complete JSON string"))?;
            if arguments.len() > 8192 {
                return Err(invalid("tool arguments exceed 8192-byte limit"));
            }
            let arguments: Value = serde_json::from_str(arguments)
                .map_err(|_| invalid("tool arguments are not complete valid JSON"))?;
            if !arguments.is_object() {
                return Err(invalid("function tool arguments must be a JSON object"));
            }
            tool_calls.push(ToolCall { id:id.to_owned(), name:name.to_owned(), arguments });
        }
    }
    let finish_reason = match choice.get("finish_reason").and_then(Value::as_str) {
        Some("stop") => FinishReason::Stop,
        Some("tool_calls" | "function_call") => FinishReason::ToolCall,
        Some("length") => FinishReason::MaxOutput,
        Some("content_filter") => FinishReason::ContentFilter,
        Some(other) if other.len() <= 128 => FinishReason::Other(other.to_owned()),
        Some(_) => return Err(invalid("provider finish_reason exceeds byte limit")),
        None => return Err(invalid("provider response lacks a terminal finish_reason")),
    };
    if finish_reason == FinishReason::ToolCall && tool_calls.is_empty() {
        return Err(invalid("tool finish reason has no complete tool calls"));
    }
    if finish_reason == FinishReason::Stop && !tool_calls.is_empty() {
        return Err(invalid("stop finish reason contains tool calls"));
    }
    let reasoning = ["reasoning_content", "reasoning", "reasoning_text"].into_iter()
        .find_map(|field| message.get(field).and_then(Value::as_str))
        .filter(|text| !text.is_empty())
        .map(|text| Reasoning { text:Some(text.to_owned()) });
    if reasoning.as_ref().and_then(|value| value.text.as_ref()).is_some_and(|text| text.contains('\0')) {
        return Err(invalid("reasoning text contains NUL"));
    }
    let usage = body.get("usage").filter(|value| !value.is_null())
        .or_else(|| choice.get("usage"));
    let usage = usage.map(|usage| Usage {
        prompt_tokens: usage.get("prompt_tokens").and_then(Value::as_u64),
        completion_tokens: usage.get("completion_tokens").and_then(Value::as_u64),
        total_tokens: usage.get("total_tokens").and_then(Value::as_u64),
        cache_read_tokens: usage.get("prompt_tokens_details")
            .and_then(|value| value.get("cached_tokens")).and_then(Value::as_u64),
        cache_write_tokens: usage.get("cache_creation_input_tokens").and_then(Value::as_u64),
    }).unwrap_or_else(Usage::unknown);
    // The upstream loop rejects incomplete/filtered finishes and performs call
    // ID, tool availability, historical reuse and final-text validation itself.
    Ok(ModelResponse { content, tool_calls, usage, finish_reason, reasoning })
}

#[derive(Default)]
pub(crate) struct CallbackWorker {
    thread: Mutex<Option<JoinHandle<()>>>,
}

impl CallbackWorker {
    pub fn join(&self) -> Result<()> {
        let mut thread = self.thread.lock()
            .map_err(|_| invalid("native callback worker lock failed"))?;
        if let Some(thread) = thread.take() {
            thread.join().map_err(|_| invalid("native callback worker failed"))?;
        }
        Ok(())
    }
}

impl Drop for CallbackWorker {
    fn drop(&mut self) {
        // Also drain on early returns and when an async deadline drops the wait.
        let _ = self.join();
    }
}

pub(crate) async fn call_callback(
    worker: &CallbackWorker,
    request: Vec<u8>, callback: ModelCallback, context: usize, capacity: usize,
) -> Result<Vec<u8>> {
    let receiver = {
        // Model and tool callbacks are serial. Reap the previous thread before
        // allocating another internal stack.
        worker.join()?;
        let mut thread = worker.thread.lock()
            .map_err(|_| invalid("native callback worker lock failed"))?;
        let (sender, receiver) = oneshot::channel();
        let builder = thread::Builder::new().name("unravel-https".into());
        // Keep cache-disabled native work on an internal stack. The measured
        // HTTPS path used <4 KiB; 12 KiB retains headroom without a 24 KiB block.
        #[cfg(target_os = "espidf")]
        let builder = builder.stack_size(12 * 1024);
        *thread = Some(builder.spawn(move || {
            let result = (|| -> Result<Vec<u8>> {
                let mut response = vec![0u8; capacity];
                // SAFETY: RPC joins this worker before returning, even if its
                // async wait times out. Both vectors are owned by this thread.
                let length = unsafe { callback(request.as_ptr(), request.len(),
                    response.as_mut_ptr(), response.len(), context as *mut c_void) };
                if length < 0 {
                    return Err(ModelError::transient(format!("native callback failed ({length})")).into());
                }
                let length = length as usize;
                if length == 0 || length > response.len() {
                    return Err(invalid("native callback returned an invalid response length"));
                }
                response.truncate(length);
                Ok(response)
            })();
            let _ = sender.send(result);
        }).map_err(|error| invalid(format!("could not start native callback worker: {error}")))?);
        receiver
    };
    receiver.await.map_err(|_| invalid("native callback worker failed"))?
}

#[async_trait]
impl Model for CallbackModel {
    async fn complete(&self, request: ModelRequest) -> Result<ModelResponse> {
        let history_bytes = encoded_len(&request.messages, SESSION_LIMIT.saturating_sub(4096))
            .map_err(|_| invalid("session has insufficient space for another model turn"))?;
        let request = payload(&self.model, request)?;
        let response = call_callback(&self.worker, request, self.callback, self.context, MODEL_RESPONSE_LIMIT).await?;
        let response = parse_response(&response)?;
        let remaining = SESSION_LIMIT.saturating_sub(history_bytes + 4096);
        // Response includes non-persisted usage/finish fields; a conservative
        // encoded bound avoids cloning the assembled assistant output.
        let assistant_bytes = encoded_len(&response, remaining.saturating_sub(32))
            .map_err(|_| invalid("assistant response exceeds remaining session byte budget"))?;
        // Reserve space for IDs/session framing and bounded error tool results.
        // The actual core still owns history insertion, tool order and recovery.
        self.history_budget.store(remaining.saturating_sub(assistant_bytes + 32), Ordering::Relaxed);
        Ok(response)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::mpsc;
    use std::time::Duration;

    struct Context {
        started: Option<oneshot::Sender<()>>,
        release: mpsc::Receiver<()>,
        finished: bool,
    }

    unsafe extern "C" fn gated_callback(
        _: *const u8, _: usize, response: *mut u8, _: usize, context: *mut c_void,
    ) -> i32 {
        let context = unsafe { &mut *(context as *mut Context) };
        let _ = context.started.take().unwrap().send(());
        context.release.recv().unwrap();
        context.finished = true;
        unsafe { response.write(b'x'); }
        1
    }

    #[test]
    fn cancelled_callback_is_joined_before_context_is_released() {
        let (started_tx, started_rx) = oneshot::channel();
        let (release_tx, release_rx) = mpsc::channel();
        let mut context = Box::new(Context {
            started: Some(started_tx), release: release_rx, finished: false,
        });
        let address = (&mut *context) as *mut Context as usize;
        let worker = CallbackWorker::default();
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_time().build().unwrap();
        runtime.block_on(async {
            let callback = call_callback(&worker, vec![], gated_callback, address, 1);
            tokio::pin!(callback);
            tokio::select! {
                result = &mut callback => panic!("callback completed before release: {result:?}"),
                result = started_rx => result.unwrap(),
            }
            assert!(tokio::time::timeout(Duration::ZERO, &mut callback).await.is_err());
        });
        drop(runtime);
        let (dropping_tx, dropping_rx) = mpsc::channel();
        let (dropped_tx, dropped_rx) = mpsc::channel();
        let joiner = thread::spawn(move || {
            dropping_tx.send(()).unwrap();
            drop(worker);
            dropped_tx.send(()).unwrap();
        });
        dropping_rx.recv().unwrap();
        assert_eq!(dropped_rx.recv_timeout(Duration::from_millis(50)),
            Err(mpsc::RecvTimeoutError::Timeout));
        release_tx.send(()).unwrap();
        dropped_rx.recv_timeout(Duration::from_secs(5)).unwrap();
        joiner.join().unwrap();
        // Reading caller-owned context is safe only after the worker was joined.
        assert!(context.finished);
    }
}
