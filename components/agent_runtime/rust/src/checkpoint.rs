use crate::{bounded_json, encoded_len, SESSION_LIMIT};
use serde::{Serialize, Serializer, ser::SerializeSeq};
use unravel_agent_runtime::{Error, Message, Result, Session};

pub const CONTEXT_BYTES: usize = 24 * 1024;
pub const CONTEXT_MESSAGES: usize = 48;
pub const MAX_OFFSET: u64 = 9_007_199_254_740_991;

// Verify each assistant call batch and its contiguous results as a single unit.
// Unknown is valid archived evidence, but is not an evictable/resumable result.
pub fn valid_groups(messages: &[Message]) -> bool {
    let mut index = 0;
    while index < messages.len() {
        match &messages[index] {
            Message::Assistant { tool_calls, .. } if !tool_calls.is_empty() => {
                if tool_calls.len() > 8 { return false; }
                for (call_index, call) in tool_calls.iter().enumerate() {
                    if call.id.is_empty() || !call.arguments.is_object() ||
                        tool_calls[..call_index].iter().any(|prior| prior.id == call.id) ||
                        messages[..index].iter().any(|message| matches!(message,
                            Message::Assistant { tool_calls, .. } if tool_calls.iter().any(|prior| prior.id == call.id))) {
                        return false;
                    }
                }
                let mut matched = [false; 8];
                index += 1;
                while index < messages.len() {
                    let (id, name) = match &messages[index] {
                        Message::Tool { call_id, name, .. } | Message::ToolUnknown { call_id, name, .. } =>
                            (call_id.as_str(), name.as_str()),
                        _ => break,
                    };
                    let Some(call_index) = tool_calls.iter().position(|call| call.id == id && call.name == name) else {
                        return false;
                    };
                    if matched[call_index] { return false; }
                    matched[call_index] = true;
                    index += 1;
                }
                if matched[..tool_calls.len()].iter().any(|matched| !matched) { return false; }
            }
            Message::Tool { .. } | Message::ToolUnknown { .. } => return false,
            _ => index += 1,
        }
    }
    true
}

#[derive(Serialize)]
struct Window<'a> {
    id: &'a str,
    messages: WindowMessages<'a>,
}

struct WindowMessages<'a> {
    head: &'a [Message],
    tail: &'a [Message],
}
impl Serialize for WindowMessages<'_> {
    fn serialize<S: Serializer>(&self, serializer: S) -> std::result::Result<S::Ok, S::Error> {
        let mut sequence = serializer.serialize_seq(Some(self.head.len() + self.tail.len()))?;
        for message in self.head.iter().chain(self.tail) { sequence.serialize_element(message)?; }
        sequence.end()
    }
}

fn window_fits(session: &Session, prefix: usize, drop_end: usize) -> bool {
    let count = prefix + session.messages.len() - drop_end;
    if count > CONTEXT_MESSAGES { return false; }
    let window = Window {
        id: &session.id,
        messages: WindowMessages { head: &session.messages[..prefix], tail: &session.messages[drop_end..] },
    };
    encoded_len(&window, CONTEXT_BYTES).is_ok()
}

/// Admit a bounded recent window. Archive the original finalized session before
/// mutation. On any error no messages or offset change. The archive callback
/// filters reasoning/secrets and gives an actual transaction acknowledgment.
/// None explicitly selects bounded ephemeral operation (no archive claim).
pub fn checkpoint(
    session: &mut Session,
    offset: &mut u64,
    mut archive: Option<impl FnMut(&[u8], u64) -> Result<()>>,
) -> Result<()> {
    if session.messages.len() > 128 || encoded_len(session, SESSION_LIMIT).is_err() || *offset > MAX_OFFSET {
        return Err(Error::InvalidInput("session exceeds hard checkpoint limits".into()));
    }
    if !valid_groups(&session.messages) {
        return Err(Error::Reconciliation("checkpoint contains incomplete or invalid tool groups".into()));
    }
    let prefix = session.messages.iter().take_while(|message| matches!(message, Message::System { .. })).count();
    let mut end = prefix;
    if !window_fits(session, prefix, end) {
        // A user starts a whole request/assistant/tool group. Never slice inside
        // a call batch, and never evict unknown effects from active context.
        let candidates = session.messages.iter().enumerate().skip(prefix + 1)
            .filter_map(|(index, message)| matches!(message, Message::User { .. }).then_some(index));
        let mut found = false;
        for candidate in candidates {
            if session.messages[prefix..candidate].iter().any(|message| matches!(message, Message::ToolUnknown { .. })) {
                break;
            }
            if window_fits(session, prefix, candidate) { end = candidate; found = true; break; }
        }
        if !found {
            return Err(Error::InvalidInput("current complete turn group exceeds 24 KiB/48 messages or unknown outcome prevents eviction".into()));
        }
    }
    let removed = end - prefix;
    let next_offset = offset.checked_add(removed as u64).filter(|value| *value <= MAX_OFFSET)
        .ok_or_else(|| Error::InvalidInput("message offset exhausted".into()))?;
    if let Some(archive) = archive.as_mut() {
        let encoded = bounded_json(session, SESSION_LIMIT)
            .map_err(|_| Error::InvalidInput("checkpoint JSON exceeds hard limit".into()))?;
        archive(&encoded, *offset)?;
    }
    session.messages.drain(prefix..end);
    *offset = next_offset;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use unravel_agent_runtime::ToolCall;

    fn group(session: &mut Session, id: usize) {
        session.messages.push(Message::user_text(format!("question {id}")));
        session.messages.push(Message::Assistant {
            content: String::new(), reasoning: None,
            tool_calls: vec![ToolCall { id: format!("call-{id}"), name: "read".into(), arguments: json!({}) }],
        });
        session.messages.push(Message::Tool {
            call_id: format!("call-{id}"), name: "read".into(), content: "evidence".into(), is_error: false,
        });
        session.messages.push(Message::assistant_text("answer"));
    }
    #[test]
    fn thousand_groups_have_stable_keys_and_bounded_working_state() {
        let mut session = Session::new("conversation");
        session.messages.push(Message::System { content: "instructions".into() });
        let mut offset = 0;
        let mut last_key = 0;
        for turn in 0..1000 {
            group(&mut session, turn);
            checkpoint(&mut session, &mut offset, Some(|bytes: &[u8], archived_offset: u64| {
                let saved: Session = serde_json::from_slice(bytes).unwrap();
                assert!(valid_groups(&saved.messages));
                let key = archived_offset + saved.messages.len() as u64 - 1;
                assert!(key >= last_key);
                last_key = key;
                Ok(())
            })).unwrap();
            assert!(session.messages.len() <= CONTEXT_MESSAGES);
            assert!(encoded_len(&session, CONTEXT_BYTES).is_ok());
            assert!(valid_groups(&session.messages));
        }
        assert_eq!(last_key, 4000);
        assert!(offset > 3900);
    }
    #[test]
    fn failed_archive_keeps_entire_group_and_offset() {
        let mut session = Session::new("s");
        for i in 0..15 { group(&mut session, i); }
        let original = bounded_json(&session, SESSION_LIMIT).unwrap();
        let mut offset = 40;
        assert!(checkpoint(&mut session, &mut offset, Some(|_: &[u8], _: u64| Err(Error::Tool("sync failed".into())))).is_err());
        assert_eq!(offset, 40);
        assert_eq!(bounded_json(&session, SESSION_LIMIT).unwrap(), original);
    }
    #[test]
    fn byte_budget_and_oversized_current_group_are_atomic() {
        let mut session = Session::new("s");
        for _ in 0..8 {
            session.messages.push(Message::user_text("request"));
            session.messages.push(Message::assistant_text("x".repeat(4096)));
        }
        let mut offset = 0;
        checkpoint(&mut session, &mut offset, Some(|_: &[u8], _: u64| Ok(()))).unwrap();
        assert!(offset > 0);
        assert!(encoded_len(&session, CONTEXT_BYTES).is_ok());
        session.messages.push(Message::user_text("huge"));
        session.messages.push(Message::assistant_text("x".repeat(CONTEXT_BYTES)));
        let before = bounded_json(&session, SESSION_LIMIT).unwrap();
        assert!(checkpoint(&mut session, &mut offset, Some(|_: &[u8], _: u64| panic!("cannot archive inadmissible group"))).is_err());
        assert_eq!(bounded_json(&session, SESSION_LIMIT).unwrap(), before);
    }
    #[test]
    fn unknown_and_incomplete_calls_cannot_be_evicted() {
        let mut session = Session::new("s");
        group(&mut session, 0);
        session.messages[2] = Message::ToolUnknown { call_id:"call-0".into(), name:"read".into(), reason:"power lost".into() };
        for i in 1..15 { group(&mut session, i); }
        assert!(checkpoint(&mut session, &mut 0, Some(|_: &[u8], _: u64| Ok(()))).is_err());
        session.messages.remove(2);
        assert!(!valid_groups(&session.messages));
    }
}
