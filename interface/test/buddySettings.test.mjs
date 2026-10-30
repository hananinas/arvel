import assert from 'node:assert/strict';
import { after, test } from 'node:test';
import { createServer } from 'vite';
import { createServer as createHttpServer } from 'node:http';

// Vite 5 creates its WebSocket listener even with hmr:false. Own an
// ephemeral loopback server so concurrent test files never share port 24678.
const hmrServer = createHttpServer();
await new Promise((resolve) => hmrServer.listen(0, '127.0.0.1', resolve));

// Load the actual client validator and its normal aliases through Vite.
const server = await createServer({ server: { middlewareMode: true, hmr: { server: hmrServer } }, appType: 'custom' });
after(async () => {
	await server.close();
	await new Promise((resolve, reject) => hmrServer.close((error) => error ? reject(error) : resolve()));
});
const { isBuddySettings, validateSettingsPatch } = await server.ssrLoadModule('/src/lib/buddySettings.ts');
const { isAgentSnapshot } = await server.ssrLoadModule('/src/lib/agent.ts');

function response() {
	return {
		chat: { provider: 'ollama', endpoint: 'https://ollama.com/v1/chat/completions', model: 'fixture:cloud', apiKeyConfigured: false },
		voice: { endpoint: 'https://example.org/v1/audio/transcriptions', model: 'fixture', apiKeyConfigured: false },
		tts: { backend: 'local', voice: 'sol' },
		conversationMode: 'text_pipeline',
		live: { provider: 'nebius', model: 'nvidia/NVIDIA-NemotronLabs-VoiceChat-11B', protocol: 'nemotron_labs_realtime', endpoint: '', apiKeyConfigured: false },
		search: { provider: 'tavily', enabled: true, tavilyApiKeyConfigured: true, parallelApiKeyConfigured: false },
		buddy: { name: 'Arvel', instructions: 'Be concise.', readAloud: false, scene: 'meadow' },
		preferences: { userName: '', voiceStyle: 'warm', speechRate: 10, captions: true, brightness: 70,
			dimAtNight: true, screenSleep: '5min', persona: 'calm', volume: 60, timezone: 'UTC0' },
		busy: false
	};
}

test('settings response accepts both supported search providers', () => {
	const value = response();
	assert.equal(isBuddySettings(value), true);
	value.search.provider = 'parallel';
	assert.equal(isBuddySettings(value), true);
});

test('search metadata rejects missing, malformed and credential-bearing responses', () => {
	const missing = response();
	delete missing.search;
	assert.equal(isBuddySettings(missing), false);
	for (const change of [
		{ provider: 'unsupported' },
		{ enabled: 'true' },
		{ tavilyApiKeyConfigured: 'true' },
		{ parallelApiKeyConfigured: null },
		{ tavilyApiKey: 'must-not-be-returned' },
		{ parallelApiKey: 'must-not-be-returned' }
	]) {
		const value = response();
		Object.assign(value.search, change);
		assert.equal(isBuddySettings(value), false);
	}
});

test('adding search does not bypass existing settings validation', () => {
	const value = response();
	value.preferences.brightness = 77;
	assert.equal(isBuddySettings(value), false);
	value.preferences.brightness = 70;
	value.chat.apiKey = 'must-not-be-returned';
	assert.equal(isBuddySettings(value), false);
});

test('search key validation preserves independent provider actions and byte boundaries', () => {
	assert.deepEqual(validateSettingsPatch({ search: { provider: 'parallel', enabled: false,
		tavilyApiKey: 'x'.repeat(1024), clearParallelApiKey: true } }, response()), []);
	assert.deepEqual(validateSettingsPatch({ search: { tavilyApiKey: 'x'.repeat(1025) } }, response()).map((issue) => issue.field), ['search-tavily-key']);
	assert.deepEqual(validateSettingsPatch({ search: { parallelApiKey: 'bad\nheader' } }, response()).map((issue) => issue.field), ['search-parallel-key']);
	assert.deepEqual(validateSettingsPatch({ search: { tavilyApiKey: 'é' } }, response()).map((issue) => issue.field), ['search-tavily-key']);
});

test('conflicting clear/replacement and invalid provider are actionable field errors', () => {
	assert.deepEqual(validateSettingsPatch({ search: { parallelApiKey: 'replacement', clearParallelApiKey: true } }, response()).map((issue) => issue.field), ['search-parallel-key']);
	assert.deepEqual(validateSettingsPatch({ search: { provider: 'unsupported' } }, response()).map((issue) => issue.field), ['search-provider']);
});

test('native credentials cannot cross a provider or endpoint replacement', () => {
	const current = response();
	current.live.endpoint = 'wss://voice.example.org/v1/realtime';
	current.live.apiKeyConfigured = true;
	assert.deepEqual(validateSettingsPatch({ conversationMode: 'native_audio' }, current), []);
	for (const live of [{ provider: 'custom' }, { endpoint: 'wss://different.example.org/v1/realtime' }]) {
		assert.deepEqual(validateSettingsPatch({ live }, current).map(issue => issue.field), ['live-key']);
		assert.deepEqual(validateSettingsPatch({ live: { ...live, clearApiKey: true } }, current), []);
		assert.deepEqual(validateSettingsPatch({ live: { ...live, apiKey: 'new-owner-key' } }, current), []);
	}
	current.chat.apiKeyConfigured = true;
	assert.deepEqual(validateSettingsPatch({ chat: { provider: 'openai' } }, current).map(issue => issue.field), ['chat-key']);
});

test('native profile rejects secret disclosure, cleartext and unsupported model contracts', () => {
	const current = response();
	for (const change of [{ apiKey: 'must-not-be-returned' }, { endpoint: 'ws://voice.example.org/v1/realtime' }, { protocol: 'openai_text' }, { model: 'nvidia/nemotron-voicechat' }]) {
		const value = response();
		Object.assign(value.live, change);
		assert.equal(isBuddySettings(value), false);
	}
	assert.deepEqual(validateSettingsPatch({ conversationMode: 'native_audio' }, current).map(issue => issue.field), ['live-endpoint']);
	assert.deepEqual(validateSettingsPatch({ live: { endpoint: 'wss://voice.example.org/v1/realtime?token=secret' } }, current).map(issue => issue.field), ['live-endpoint']);
	assert.deepEqual(validateSettingsPatch({ live: { apiKey: 'key', clearApiKey: true } }, current).map(issue => issue.field), ['live-key']);
});

test('native endpoint accepts only exact DNS or IPv4 realtime ingress', () => {
	for (const endpoint of ['wss://voice.example.org/v1/realtime', 'wss://192.0.2.10:8443/v1/realtime']) {
		assert.deepEqual(validateSettingsPatch({ live: { endpoint } }, response()), []);
	}
	for (const endpoint of ['wss://[::1]/v1/realtime', 'wss://voice.example.org/v1/realtime/',
		'wss://voice.example.org:0/v1/realtime', 'wss://voice.example.org:65536/v1/realtime',
		'wss://user:password@voice.example.org/v1/realtime', 'wss://999.1.1.1/v1/realtime',
		'wss://voice.example.org/%76%31/realtime', 'wss://voice.example.org/v1/realtime#secret']) {
		assert.ok(validateSettingsPatch({ live: { endpoint } }, response()).some(issue => issue.field === 'live-endpoint'), endpoint);
	}
});

test('the retired Niko settings group is rejected in both directions', () => {
	const value = response();
	value.niko = { enabled: false };
	assert.equal(isBuddySettings(value), false);
	for (const niko of [{}, { enabled: false }, null]) {
		assert.notEqual(validateSettingsPatch({ niko }, response()).length, 0);
	}
});
function nativeSnapshot() {
	return {
		source: 'NATIVE', mode: 'native_embedded', model: 'fixture:cloud', state: 'idle', status: 'Ready', busy: false,
		provider: 'ollama', conversationMode: 'text_pipeline',
		wifiConnected: true, clockReady: true, chatConfigured: true, buddyName: 'Arvel', scene: 'meadow', readAloud: false,
		tools: [], sources: [], conversationId: '',
		nativeReadiness: { state: 'untested', error: '', exercised: false },
		storage: { state: 'missing', conversationId: '', sqliteBytes: 0, sqliteHighwater: 0, databaseBytes: 0, durable: false },
		voiceConfigured: true, voicePreview: false, voiceLiveAvailable: false, voiceActive: false, voiceMuted: true,
		voiceConnecting: false, voicePhase: 'off', voiceTranscript: null, lastError: '', messageRevision: 1,
		messages: [{ id: 1, role: 'user', text: 'Native fixture', time: '12:00' }], preferences: response().preferences,
		ssid: 'Test network', ip: '192.0.2.1', firmware: 'test', localTime: '12:00',
		screenOff: false, internalFree: 65536, internalLargest: 32768, psramFree: 1048576
	};
}

test('native snapshot validates real fields without fabricating missing preferences', () => {
	const value = nativeSnapshot();
	assert.equal(isAgentSnapshot(value), true);
	delete value.preferences;
	assert.equal(isAgentSnapshot(value), false);
	assert.equal('preferences' in value, false);
	const legacy = nativeSnapshot();
	legacy.source = 'LEGACY_HTTP';
	assert.equal(isAgentSnapshot(legacy), false);
});

test('native screen and memory metrics reject malformed values', () => {
	for (const change of [{ screenOff: 'false' }, { internalFree: -1 }, { internalLargest: 1.5 }, { psramFree: '1048576' }]) {
		assert.equal(isAgentSnapshot({ ...nativeSnapshot(), ...change }), false);
	}
});

test('owner tools retain all outcomes and reject obsolete success flags or credential fields', () => {
	for (const state of ['running', 'succeeded', 'failed', 'unknown', 'cancelled', 'partial']) {
		const value = nativeSnapshot();
		value.tools = [{ id: 'provider-call', kind: 'timer', verb: 'start', target: 'fixture', state, error: '' }];
		assert.equal(isAgentSnapshot(value), true);
		value.tools[0].done = true;
		assert.equal(isAgentSnapshot(value), false);
	}
	const value = response();
	value.live.authorization = 'must-never-be-returned';
	assert.equal(isBuddySettings(value), false);
});
