#pragma once
#include <string>

// Native diagnostics only; not a voice/HTTP control surface. Exchange must return
// a real local WebRTC server's SDP answer to the actual SDK-generated offer.
// This uses the production peer task, answer parser and linker-wrapped handshake,
// never captures a microphone, never reads account tokens and never bypasses
// verification. reject_expected deliberately corrupts only the expected digest:
// a successful real handshake MUST then return the fixed fingerprint failure.
using speech_live_peer_exchange_t = bool (*)(const char *offer, std::string &answer,
                                             void *context, std::string &error);
bool speech_live_peer_security_smoke(speech_live_peer_exchange_t exchange,
                                    void *context, bool reject_expected,
                                    std::string &error);
