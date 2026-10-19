import assert from 'node:assert/strict';
import { after, test } from 'node:test';
import { createServer } from 'vite';
import { createServer as createHttpServer } from 'node:http';

// Vite 5 creates its WebSocket listener even with hmr:false. Own an
// ephemeral loopback server so concurrent test files never share port 24678.
const hmrServer = createHttpServer();
await new Promise((resolve) => hmrServer.listen(0, '127.0.0.1', resolve));

// Exercise the actual browser module using the project's normal Vite loader.
const server = await createServer({ server: { middlewareMode: true, hmr: { server: hmrServer } }, appType: 'custom' });
after(async () => {
	await server.close();
	await new Promise((resolve, reject) => hmrServer.close((error) => error ? reject(error) : resolve()));
});
const {
	parseModelCatalog, implementedCapability, catalogTextEndpoint,
	CODEX_LIVE_MODEL_ID, NEMOTRON_LABS_MODEL_ID
} = await server.ssrLoadModule('/src/lib/modelCatalog.ts');

function model(providerId = 'nebius', fields = {}) {
	return parseModelCatalog({ [providerId]: { models: { fixture: fields } } }, 123).providers[0].models[0];
}

test('provider scope owns identity and explicit model identity takes precedence over map keys', () => {
	const catalog = parseModelCatalog({
		nebius: { id: 'incorrect-provider', name: 'Nebius Token Factory', models: {
			alias: { id: 'exact/model', name: 'Exact' }, fallback: { id: '' }, missing: {}
		} },
		openai: { models: { shared: { id: 'exact/model' } } }
	}, 123);
	assert.equal(catalog.fetchedAt, 123);
	assert.equal(catalog.providers[0].id, 'nebius');
	assert.deepEqual(catalog.providers[0].models.map((item) => [item.providerId, item.id]), [
		['nebius', 'exact/model'], ['nebius', 'fallback'], ['nebius', 'missing']
	]);
	assert.equal(catalog.providers[1].models[0].id, 'exact/model');
	assert.equal(catalog.providers[1].models[0].providerId, 'openai');
});

test('duplicate IDs resolve deterministically within one provider, never across providers', () => {
	const first = { z: { id: 'same', name: 'Second' }, a: { id: 'same', name: 'First' } };
	const second = { a: first.a, z: first.z };
	const parsed = (models) => parseModelCatalog({ nebius: { models } }, 123);
	assert.deepEqual(parsed(first), parsed(second));
	assert.equal(parsed(first).providers[0].models.length, 1);
	assert.equal(parsed(first).providers[0].models[0].name, 'First');
});

test('unknown capabilities, modalities, token limits and prices stay unknown rather than false or free', () => {
	const entry = model();
	assert.equal(entry.tools, null);
	assert.equal(entry.input, null);
	assert.equal(entry.output, null);
	assert.equal(entry.status, null);
	assert.equal(entry.context, null);
	assert.equal(entry.maxOutput, null);
	assert.deepEqual(entry.pricing, { input: null, output: null, cacheRead: null });
	const malformed = model('nebius', {
		tool_call: 'true', modalities: { input: ['text', 1], output: null },
		limit: { context: -1, output: 0 }, cost: { input: '0', output: -1, cache_read: Infinity }
	});
	assert.equal(malformed.tools, null);
	assert.equal(malformed.input, null);
	assert.equal(malformed.output, null);
	assert.equal(malformed.context, null);
	assert.equal(malformed.maxOutput, null);
	assert.deepEqual(malformed.pricing, { input: null, output: null, cacheRead: null });
});

test('explicit tool support, zero pricing and valid limits retain their actual values', () => {
	const entry = model('nebius', {
		status: 'beta', tool_call: true,
		modalities: { input: ['text', 'audio', 'text'], output: ['text'] },
		limit: { context: 32768, output: 1024 }, cost: { input: 0, output: 1.5, cache_read: 0.25 }
	});
	assert.equal(entry.status, 'beta');
	assert.equal(entry.tools, true);
	assert.deepEqual(entry.input, ['text', 'audio']);
	assert.deepEqual(entry.output, ['text']);
	assert.equal(entry.context, 32768);
	assert.equal(entry.maxOutput, 1024);
	assert.deepEqual(entry.pricing, { input: 0, output: 1.5, cacheRead: 0.25 });
	assert.deepEqual(model('nebius', { modalities: { input: [], output: [] } }).input, []);
});

test('malformed entries do not destroy valid discovery or impersonate another provider', () => {
	const catalog = parseModelCatalog({
		broken: null,
		badModels: { models: [] },
		nebius: { models: {
			badId: { id: 42 }, badStatus: { status: false }, badShape: 'model',
			valid: { providerId: 'openai', id: 'stable', tool_call: false }
		} }
	}, 123);
	assert.equal(catalog.providers.length, 1);
	assert.equal(catalog.providers[0].models.length, 1);
	assert.equal(catalog.providers[0].models[0].providerId, 'nebius');
	assert.equal(catalog.providers[0].models[0].tools, false);
});

test('invalid whole catalog, empty catalog and a catalog without valid entries are distinct', () => {
	for (const input of [null, [], 'catalog', 5]) {
		assert.throws(() => parseModelCatalog(input), { code: 'invalid_catalog' });
	}
	assert.throws(() => parseModelCatalog({}), { code: 'empty_catalog' });
	assert.throws(() => parseModelCatalog({ broken: { models: { entry: null } } }), { code: 'no_valid_entries' });
	assert.throws(() => parseModelCatalog({ nebius: { models: { entry: {} } } }, NaN), { code: 'invalid_catalog' });
	// Valid discovery is not the same as an implemented adapter.
	const unsupported = model('unimplemented-sdk', { tool_call: true });
	assert.equal(implementedCapability('unimplemented-sdk', unsupported).usable, false);
});

test('sanitization strips raw provider configuration and credential-bearing endpoints', () => {
	const catalog = parseModelCatalog({ nebius: {
		api: 'https://owner:secret@example.org/v1', env: ['PRIVATE_KEY'], npm: '@provider/sdk',
		apiKey: 'owner-secret', models: { fixture: { apiKey: 'another-secret', sdk: { execute: true } } }
	} }, 123);
	assert.equal(catalog.providers[0].api, null);
	assert.deepEqual(Object.keys(catalog.providers[0]), ['id', 'name', 'api', 'models']);
	const encoded = JSON.stringify(catalog);
	for (const excluded of ['owner-secret', 'another-secret', 'PRIVATE_KEY', '@provider/sdk', 'execute']) {
		assert.equal(encoded.includes(excluded), false);
	}
	for (const api of ['https://example.org/v1?key=secret', 'https://example.org/v1#secret', 'http://example.org/v1']) {
		assert.equal(parseModelCatalog({ nebius: { api, models: { fixture: {} } } }).providers[0].api, null);
	}
});

test('text endpoint mapping uses only explicit adapters and an actual safe catalog base', () => {
	for (const id of ['openai', 'nebius', 'ollama', 'openrouter', 'groq', 'togetherai', 'fireworks-ai', 'deepinfra', 'xai', 'nvidia']) {
		assert.equal(catalogTextEndpoint({ id, api: 'https://example.org/v1/' }), 'https://example.org/v1/chat/completions');
	}
	assert.equal(catalogTextEndpoint({ id: 'nebius', api: 'https://example.org/v1/chat/completions' }), 'https://example.org/v1/chat/completions');
	assert.equal(catalogTextEndpoint({ id: 'unknown', api: 'https://example.org/v1' }), null);
	assert.equal(catalogTextEndpoint({ id: 'openai', api: null }), null);
	assert.equal(catalogTextEndpoint({ id: 'openai', api: 'https://example.org/v1?key=secret' }), null);
});

test('deprecated models, explicit lack of tools and mismatched scope are unusable', () => {
	assert.equal(implementedCapability('nebius', model('nebius', { status: 'deprecated', tool_call: true })).usable, false);
	assert.equal(implementedCapability('nebius', model('nebius', { tool_call: false })).usable, false);
	assert.equal(implementedCapability('openai', model('nebius', { tool_call: true })).usable, false);
});

test('audio input, audio output and realtime-looking names never invent native speech transport', () => {
	const inputOnly = model('nvidia', {
		id: 'nvidia/nemotron-voicechat', tool_call: true,
		modalities: { input: ['text', 'audio'], output: ['text'] }
	});
	assert.equal(implementedCapability('nvidia', inputOnly).speech, false);
	assert.equal(implementedCapability('nvidia', inputOnly, 'nemotron_labs_realtime').usable, false);
	const both = model('nebius', {
		id: 'magic-realtime-voicechat', tool_call: true,
		modalities: { input: ['audio'], output: ['audio'] }
	});
	assert.equal(implementedCapability('nebius', both).usable, false);
	assert.equal(implementedCapability('nebius', both).speech, false);
	assert.equal(implementedCapability('nebius', both, 'nemotron_labs_realtime').usable, false);
});

test('documented Labs override requires exact artifact, explicit protocol and self-hosted provider', () => {
	const labs = model('nebius', { id: NEMOTRON_LABS_MODEL_ID });
	const supported = implementedCapability('nebius', labs, 'nemotron_labs_realtime');
	assert.equal(supported.usable, true);
	assert.equal(supported.speech, true);
	assert.match(supported.reason, /[Ss]elf-hosted/);
	assert.match(supported.reason, /not an official Nebius Token Factory listing/);
	assert.match(supported.reason, /different 12B/);
	assert.equal(implementedCapability('nebius', labs).speech, false);
	assert.equal(implementedCapability('nebius', labs, 'openai_text').usable, false);
	assert.match(implementedCapability('nebius', labs, 'openai_text').reason, /typed conversation/);
	assert.equal(implementedCapability('nebius', null, 'nemotron_labs_realtime').usable, false);
	assert.equal(implementedCapability('groq', model('groq', { id: NEMOTRON_LABS_MODEL_ID }), 'nemotron_labs_realtime').usable, false);
	assert.equal(implementedCapability('nebius', model('nebius', { id: NEMOTRON_LABS_MODEL_ID.toLowerCase() }), 'nemotron_labs_realtime').usable, false);
});

test('Codex speech is an exact separate account integration, not a generic catalog model', () => {
	const codex = model('openai', { id: CODEX_LIVE_MODEL_ID });
	assert.equal(implementedCapability('openai', codex, 'codex_live').speech, true);
	assert.equal(implementedCapability('openai', codex).speech, false);
	assert.equal(implementedCapability('openai', null, 'codex_live').usable, false);
	assert.equal(implementedCapability('openai', model('openai', { id: 'gpt-realtime' }), 'codex_live').usable, false);
	assert.equal(implementedCapability('nebius', model('nebius', { id: CODEX_LIVE_MODEL_ID }), 'codex_live').usable, false);
});

test('saved custom text deployments remain usable offline without upgrading unknown metadata', () => {
	for (const provider of ['custom', 'nebius', 'ollama']) {
		const capability = implementedCapability(provider, null);
		assert.equal(capability.usable, true);
		assert.equal(capability.speech, false);
		assert.match(capability.reason, /Verify/);
	}
	assert.equal(implementedCapability('owner-self-hosted', null, 'openai_text').usable, true);
	assert.equal(implementedCapability('owner-self-hosted', null).usable, false);
	assert.equal(implementedCapability('owner-self-hosted', model('owner-self-hosted'), 'openai_text').usable, false);
	assert.equal(implementedCapability('', null, 'openai_text').usable, false);
	const unknown = model();
	assert.equal(implementedCapability('nebius', unknown).usable, true);
	assert.match(implementedCapability('nebius', unknown).reason, /unknown/);
	assert.equal(unknown.tools, null);
	assert.equal(unknown.input, null);
});
