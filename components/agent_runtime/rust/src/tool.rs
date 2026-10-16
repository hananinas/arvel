use crate::{bounded_json, encoded_len, model::{call_callback, CallbackWorker}, ModelCallback, MODEL_TEXT_LIMIT};
use async_trait::async_trait;
use serde::Deserialize;
use serde_json::{json, Value};
use std::sync::{Arc, atomic::{AtomicUsize, Ordering}};
use unravel_agent_runtime::{Error, Result, Tool, ToolCallContext, ToolDefinition, ToolOutput, ToolState};

pub(crate) struct CallbackTool {
    pub definition: ToolDefinition,
    pub callback: ModelCallback,
    pub clock: crate::ClockCallback,
    pub context: usize,
    pub worker: Arc<CallbackWorker>,
    pub history_budget: Arc<AtomicUsize>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct CallbackOutput {
    content: String,
    #[serde(default)]
    metadata: Value,
    state: ToolState,
}

fn unknown(reason: &str) -> ToolOutput {
    let mut result = ToolOutput::text(reason);
    result.state = ToolState::Unknown;
    result
}

#[async_trait]
impl Tool for CallbackTool {
    fn definition(&self) -> ToolDefinition {
        self.definition.clone()
    }

    async fn execute(&self, arguments: Value, call: &ToolCallContext<'_>) -> Result<ToolOutput> {
        if call.stop.is_stopped() { return Err(Error::Stopped); }
        if call.deadline.is_some_and(|deadline| tokio::time::Instant::now() >= deadline) {
            return Err(Error::DeadlineExceeded);
        }
        let remaining = call.deadline.map(|deadline| deadline.saturating_duration_since(tokio::time::Instant::now()).as_micros() as u64);
        let native_now = unsafe { (self.clock)(self.context as *mut std::ffi::c_void) };
        let deadline_us = remaining.map(|remaining| native_now.saturating_add(remaining));
        let request = bounded_json(&json!({
            "name":self.definition.name,"arguments":arguments,
            "call_id":call.call_id, "run_id":call.identity.run_id,
            "attempt":call.attempt, "deadline_us":deadline_us,
            "conversation_id":call.identity.conversation_id,
            "provider_session":call.identity.provider_session,
            "generation":call.identity.generation, "origin":call.identity.origin,
        }), 16 * 1024).map_err(|_| Error::Tool("tool request exceeds byte limit".into()))?;
        let response = match call_callback(&self.worker, request, self.callback, self.context, 32 * 1024).await {
            Ok(response) => response,
            Err(_) => return Ok(unknown("tool callback transport failed after dispatch; effect unknown")),
        };
        let output: CallbackOutput = match serde_json::from_slice(&response) {
            Ok(output) => output,
            Err(_) => return Ok(unknown("invalid tool callback result after dispatch; effect unknown")),
        };
        if output.content.len() > MODEL_TEXT_LIMIT {
            return Ok(unknown("tool content exceeds byte limit after dispatch; effect unknown"));
        }
        let encoded_bytes = match encoded_len(&output.content, 32 * 1024) {
            Ok(bytes) => bytes + 512,
            Err(_) => return Ok(unknown("encoded tool content exceeds byte limit; effect unknown")),
        };
        if self.history_budget.fetch_update(Ordering::Relaxed, Ordering::Relaxed,
            |remaining| remaining.checked_sub(encoded_bytes)).is_err()
        {
            return Ok(unknown("tool output exceeds remaining session byte budget; effect unknown"));
        }
        let mut result = ToolOutput::with_metadata(output.content, output.metadata);
        result.state = output.state;
        Ok(result)
    }
}
