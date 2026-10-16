<script lang="ts">
	import { onMount, onDestroy } from 'svelte';
	import {
		fetchModelCatalog, loadCachedModelCatalog, implementedCapability, catalogTextEndpoint,
		CODEX_LIVE_PROVIDER_ID, CODEX_LIVE_MODEL_ID, CODEX_LIVE_ENDPOINT,
		NEMOTRON_LABS_MODEL_ID, NEMOTRON_LABS_PROVIDER_IDS, OPENAI_TEXT_PROVIDER_IDS,
		type CatalogModel, type CatalogProvider, type CatalogSnapshot, type CatalogSelection
	} from '$lib/modelCatalog';

	let { providerId, modelId, disabled = false, onselect }: {
		providerId: string;
		modelId: string;
		disabled?: boolean;
		onselect: (selection: CatalogSelection) => void;
	} = $props();

	const uid = $props.id();
	let snapshot = $state<CatalogSnapshot | null>(null);
	let loading = $state(false);
	let error = $state('');
	let source = $state<'catalog' | 'custom' | 'codex'>('catalog');
	let selectedProvider = $state('');
	let selectedModel = $state('');
	let providerSearch = $state('');
	let modelSearch = $state('');
	let customProvider = $state('');
	let customModel = $state('');
	let customProtocol = $state<'openai_text' | 'nemotron_labs_realtime'>('openai_text');
	let notice = $state('');
	let now = $state(0);
	let alive = false;
	let controller: AbortController | null = null;
	let lastProvider: string | undefined;
	let lastModel: string | undefined;

	// Only a changed owner draft resets identity; fetching metadata never changes it.
	$effect(() => {
		if (lastProvider === providerId && lastModel === modelId) return;
		lastProvider = providerId;
		lastModel = modelId;
		selectedProvider = providerId || 'custom';
		selectedModel = modelId;
		customProvider = providerId || 'custom';
		customModel = modelId;
		notice = '';
	});

	const providers = $derived.by(() => {
		const entries = new Map<string, CatalogProvider>();
		for (const id of OPENAI_TEXT_PROVIDER_IDS) {
			entries.set(id, { id, name: id === 'nebius' ? 'Nebius · custom or Token Factory' : id === 'custom' ? 'Custom provider' : id, api: null, models: [] });
		}
		for (const entry of snapshot?.providers ?? []) entries.set(entry.id, entry);
		if (selectedProvider && !entries.has(selectedProvider)) {
			entries.set(selectedProvider, { id: selectedProvider, name: `${selectedProvider} · saved/custom`, api: null, models: [] });
		}
		return [...entries.values()].sort((a, b) => a.name.localeCompare(b.name));
	});
	const visibleProviders = $derived(providers.filter((entry) => entry.id === selectedProvider ||
		`${entry.name} ${entry.id}`.toLowerCase().includes(providerSearch.trim().toLowerCase())));
	const provider = $derived(providers.find((entry) => entry.id === selectedProvider) ?? null);
	const model = $derived(provider?.models.find((entry) => entry.id === selectedModel) ?? null);
	const visibleModels = $derived((provider?.models ?? []).filter((entry) => entry.id === selectedModel ||
		`${entry.name} ${entry.id}`.toLowerCase().includes(modelSearch.trim().toLowerCase())));
	const catalogCapability = $derived(implementedCapability(selectedProvider, model, 'openai_text'));
	const catalogEndpoint = $derived(provider ? catalogTextEndpoint(provider) : null);
	const nativeProvider = $derived(selectedProvider === 'custom' ? customProvider.trim() : selectedProvider);
	const labsModel: CatalogModel = {
		providerId: '', id: NEMOTRON_LABS_MODEL_ID, name: 'Released NemotronLabs VoiceChat 11B · self-hosted',
		status: null, tools: null, input: null, output: null, context: null, maxOutput: null,
		pricing: { input: null, output: null, cacheRead: null }
	};
	const customCapability = $derived(implementedCapability(nativeProvider,
		customProtocol === 'nemotron_labs_realtime' || customModel.trim() === NEMOTRON_LABS_MODEL_ID
			? { ...labsModel, providerId: nativeProvider } : null,
		customProtocol));
	const nativeAllowed = $derived(NEMOTRON_LABS_PROVIDER_IDS.some((id) => id === nativeProvider));
	const customReady = $derived(!!nativeProvider && customCapability.usable &&
		(customProtocol === 'nemotron_labs_realtime' || !!customModel.trim()));
	const fetchedDate = $derived(snapshot ? new Date(snapshot.fetchedAt).toLocaleString() : '');
	const cacheAge = $derived.by(() => {
		if (!snapshot) return '';
		const minutes = Math.max(0, Math.floor((now - snapshot.fetchedAt) / 60_000));
		if (minutes < 1) return 'less than a minute old';
		if (minutes < 60) return `${minutes} minute${minutes === 1 ? '' : 's'} old`;
		const hours = Math.floor(minutes / 60);
		if (hours < 24) return `${hours} hour${hours === 1 ? '' : 's'} old`;
		const days = Math.floor(hours / 24);
		return `${days} day${days === 1 ? '' : 's'} old`;
	});

	async function refreshCatalog() {
		if (!alive || loading) return;
		const request = new AbortController();
		controller = request;
		loading = true;
		error = '';
		try {
			const catalog = await fetchModelCatalog(request.signal);
			if (!alive || request.signal.aborted) return;
			snapshot = catalog;
			now = Date.now();
		} catch {
			if (!alive || request.signal.aborted) return;
			error = 'The model catalog could not be refreshed. Your draft is unchanged. Use cached metadata or a custom/self-hosted model, or retry.';
		} finally {
			if (alive && controller === request) {
				loading = false;
				controller = null;
			}
		}
	}

	function chooseProvider(value: string) {
		selectedProvider = value;
		selectedModel = value === providerId ? modelId : '';
		modelSearch = '';
		notice = '';
	}

	function applySelection(selection: CatalogSelection) {
		if (disabled) return;
		onselect(selection);
		notice = 'Selection added to your draft. Review its endpoint and credentials, then save in Settings. The microphone remains off.';
	}

	function useCatalog() {
		if (disabled || !model || !catalogCapability.usable || !catalogEndpoint) return;
		applySelection({ kind: 'text_pipeline', provider: selectedProvider, model: model.id, endpoint: catalogEndpoint, protocol: 'openai_text' });
	}

	function useCustom() {
		if (disabled || !customReady) return;
		applySelection({
			kind: customProtocol === 'nemotron_labs_realtime' ? 'native_audio' : 'text_pipeline',
			provider: nativeProvider,
			model: customProtocol === 'nemotron_labs_realtime' ? NEMOTRON_LABS_MODEL_ID : customModel.trim(),
			endpoint: '', protocol: customProtocol
		});
	}

	function modalities(value: string[] | null) {
		return value === null ? 'Unknown' : value.length ? value.join(', ') : 'None listed';
	}
	function price(value: number | null) {
		return value === null ? 'Unknown' : new Intl.NumberFormat(undefined, { style: 'currency', currency: 'USD', maximumFractionDigits: 6 }).format(value);
	}

	onMount(() => {
		alive = true;
		now = Date.now();
		snapshot = loadCachedModelCatalog();
		// Catalog discovery is public metadata, independent of owner settings loading.
		void refreshCatalog();
	});
	onDestroy(() => {
		alive = false;
		controller?.abort();
	});
</script>

<section class="model-picker" aria-labelledby={`${uid}-title`}>
	<div class="card-head">
		<h3 id={`${uid}-title`}>Choose a conversation model</h3>
		<button type="button" class="a-btn ghost" disabled={disabled || loading} onclick={() => void refreshCatalog()}>{loading ? 'Refreshing catalog…' : error ? 'Retry catalog' : 'Refresh catalog'}</button>
	</div>
	<p class="draft"><strong>Current draft:</strong> {providerId || 'No provider'} / {modelId || 'No model selected'}</p>
	<p class="hint">Browse without changing your draft. Only “Use” changes the selection; it does not save, connect or start recording.</p>
	<div class="catalog-state" role="status" aria-live="polite" aria-busy={loading}>
		{#if loading}<p class="hint">Fetching public model metadata from models.dev…</p>{/if}
		{#if snapshot}<p class="hint">Sanitized catalog cache: {cacheAge}. Fetched {fetchedDate}. Catalog metadata is not a connection or deployment test.</p>
		{:else}<p class="hint">No cached catalog. Saved settings and custom/self-hosted selection remain available.</p>{/if}
		{#if error}<p class="notice error">{error}</p>{/if}
	</div>

	<fieldset disabled={disabled}>
		<legend class="sr-only">Model selection</legend>
		<div class="field">
			<label for={`${uid}-source`}>Selection source</label>
			<select id={`${uid}-source`} bind:value={source} onchange={() => (notice = '')}>
				<option value="catalog">models.dev catalog · text adapter</option>
				<option value="custom">Custom / self-hosted deployment</option>
				<option value="codex">Codex account · separate live integration</option>
			</select>
		</div>

		{#if source !== 'codex'}
			<div class="two">
				<div class="field">
					<label for={`${uid}-provider-search`}>Search providers</label>
					<input id={`${uid}-provider-search`} type="search" bind:value={providerSearch} autocomplete="off" aria-describedby={`${uid}-provider-help`} />
				</div>
				<div class="field">
					<label for={`${uid}-provider`}>Provider</label>
					<select id={`${uid}-provider`} value={selectedProvider} onchange={(event) => chooseProvider(event.currentTarget.value)} aria-describedby={`${uid}-provider-help`}>
						{#each visibleProviders as entry (entry.id)}<option value={entry.id}>{entry.name} · {entry.id}</option>{/each}
					</select>
				</div>
			</div>
			<p id={`${uid}-provider-help`} class="hint">{visibleProviders.length} providers shown. The selected provider stays visible when filtering. Custom/self-hosted is available under every provider, including Nebius.</p>
		{/if}

		{#if source === 'catalog'}
			<div class="two">
				<div class="field">
					<label for={`${uid}-model-search`}>Search models</label>
					<input id={`${uid}-model-search`} type="search" bind:value={modelSearch} autocomplete="off" aria-describedby={`${uid}-model-help`} />
				</div>
				<div class="field">
					<label for={`${uid}-model`}>Model · scoped to this provider</label>
					<select id={`${uid}-model`} bind:value={selectedModel} onchange={() => (notice = '')} aria-describedby={`${uid}-model-help`}>
						<option value="">Choose a model</option>
						{#if selectedModel && !model}<option value={selectedModel}>{selectedModel} · saved/custom, not in this catalog</option>{/if}
						{#each visibleModels as entry (entry.id)}<option value={entry.id}>{entry.name} · {entry.id}{entry.status ? ` · ${entry.status}` : ''}</option>{/each}
					</select>
				</div>
			</div>
			<p id={`${uid}-model-help`} class="hint">{visibleModels.length} models shown. Deprecated and unsupported models can be inspected, but are not enabled automatically.</p>
			{#if model}
				<div class="capabilities" aria-label="Selected model catalog metadata">
					<dl>
						<div><dt>Input</dt><dd>{modalities(model.input)}</dd></div>
						<div><dt>Output</dt><dd>{modalities(model.output)}</dd></div>
						<div><dt>Tools</dt><dd>{model.tools === null ? 'Unknown' : model.tools ? 'Supported in catalog' : 'Not supported in catalog'}</dd></div>
						<div><dt>Context tokens</dt><dd>{model.context === null ? 'Unknown' : model.context.toLocaleString()}</dd></div>
						<div><dt>Max output tokens</dt><dd>{model.maxOutput === null ? 'Unknown' : model.maxOutput.toLocaleString()}</dd></div>
						<div><dt>Status</dt><dd>{model.status ?? 'Unknown'}</dd></div>
						<div><dt>Input / 1M tokens</dt><dd>{price(model.pricing.input)}</dd></div>
						<div><dt>Output / 1M tokens</dt><dd>{price(model.pricing.output)}</dd></div>
						<div><dt>Cache read / 1M tokens</dt><dd>{price(model.pricing.cacheRead)}</dd></div>
					</dl>
					<p class="hint">Catalog prices are estimates, not your deployment bill. Unknown does not mean free.</p>
				</div>
				<p class="explanation"><strong>{catalogCapability.usable ? 'Text pipeline' : 'Not available with this adapter'}.</strong> {catalogCapability.reason}</p>
				{#if model.input?.includes('audio')}<p class="hint">Audio input is not speech output or a realtime protocol. This catalog selection does not enable native speech; text-pipeline voice uses separate transcription and read-aloud.</p>{/if}
				{#if catalogCapability.usable && !catalogEndpoint}<p class="hint">This catalog does not supply a compatible HTTPS chat endpoint. Use Custom/self-hosted and enter the exact endpoint in Settings.</p>{/if}
				{#if catalogEndpoint}<p class="hint">Text adapter endpoint: <code>{catalogEndpoint}</code>. Review credentials before saving a different provider or endpoint origin.</p>{/if}
			{:else}
				<p class="hint">{selectedModel ? 'Your saved/custom model is still available even though catalog metadata is missing. Choose Custom/self-hosted to use it without catalog lookup.' : 'Choose a model to inspect its capabilities, or use Custom/self-hosted.'}</p>
			{/if}
			<div class="actions"><button type="button" class="a-btn primary" disabled={disabled || !model || !catalogCapability.usable || !catalogEndpoint} onclick={useCatalog}>Use selected text model</button><button type="button" class="a-btn ghost" onclick={() => { source = 'custom'; notice = ''; }}>Custom / self-hosted under this provider</button></div>
		{:else if source === 'custom'}
			{#if selectedProvider === 'custom'}
				<div class="field"><label for={`${uid}-custom-provider`}>Custom provider identity</label><input id={`${uid}-custom-provider`} type="text" bind:value={customProvider} autocomplete="off" autocapitalize="off" spellcheck="false" /></div>
			{/if}
			<div class="field">
				<label for={`${uid}-protocol`}>Implemented protocol</label>
				<select id={`${uid}-protocol`} bind:value={customProtocol} aria-describedby={`${uid}-custom-help`}>
					<option value="openai_text">OpenAI-compatible text · separate speech pipeline</option>
					<option value="nemotron_labs_realtime">Released NemotronLabs VoiceChat 11B · native realtime</option>
				</select>
			</div>
			{#if customProtocol === 'openai_text'}
				<div class="field"><label for={`${uid}-custom-model`}>Custom model ID</label><input id={`${uid}-custom-model`} type="text" bind:value={customModel} autocomplete="off" autocapitalize="off" spellcheck="false" aria-describedby={`${uid}-custom-help`} /></div>
				<p id={`${uid}-custom-help`} class="hint">Use your saved or self-hosted model without a catalog match. Model tools, modalities, context, status and pricing are unknown until verified. Enter its exact HTTPS chat-completions endpoint in Settings; the picker does not replace a custom endpoint.</p>
			{:else}
				<p class="identity"><strong>Released model:</strong> <code>{NEMOTRON_LABS_MODEL_ID}</code></p>
				<p id={`${uid}-custom-help`} class="hint">Documented self-hosted Labs 11B protocol override, not an official Nebius Token Factory listing. The catalog’s <code>nvidia/nemotron-voicechat</code> is a different 12B artifact. No catalog capabilities or pricing are fabricated for this deployment.</p>
				{#if nativeAllowed && customCapability.usable}<p class="explanation"><strong>Speech provided by this model.</strong> The documented Labs protocol handles input speech, spoken replies and function calls without a separate STT/TTS model. Ordinary typed chat and standalone text synthesis are not supported.</p>{/if}
				<p class="hint">Enter your deployment’s exact authenticated <code>wss://</code> realtime URL in Settings. No deployment URL is assumed. <a href="https://github.com/NVIDIA-NeMo/Speech/tree/nemotron-labs-voicechat/voicechat_realtime_instructions" target="_blank" rel="noopener noreferrer">NVIDIA Labs protocol documentation (opens a new tab)</a>.</p>
			{/if}
			<p class="explanation"><strong>{customCapability.usable ? 'Implemented adapter' : 'Unsupported selection'}.</strong> {customCapability.reason}</p>
			<p class="hint">Custom selection preserves the endpoint draft. Review the endpoint and enter credentials for its origin in Settings. Saved custom settings remain usable during catalog outages.</p>
			<button type="button" class="a-btn primary" disabled={disabled || !customReady} onclick={useCustom}>Use custom {customProtocol === 'nemotron_labs_realtime' ? 'native speech' : 'text'} model</button>
		{:else}
			<p class="explanation"><strong>Codex account · direct live voice.</strong> This is a separate explicit integration using Arvel’s Codex sign-in, not a catalog realtime adapter or a fallback for another model.</p>
			<p class="hint">Uses the fixed account transport and model <code>{CODEX_LIVE_MODEL_ID}</code>. Account eligibility and subscription limits apply. Review Codex account setup in Settings; selecting it does not sign in or start the microphone. Device-action delegations still use your saved text profile.</p>
			<button type="button" class="a-btn primary" onclick={() => applySelection({ kind: 'codex_live', provider: CODEX_LIVE_PROVIDER_ID, model: CODEX_LIVE_MODEL_ID, endpoint: CODEX_LIVE_ENDPOINT, protocol: 'codex_live' })}>Use Codex live integration</button>
		{/if}
	</fieldset>
	<p class="hint privacy">The catalog request contains no API keys or owner endpoints. Only sanitized public metadata is cached in this browser; your selections, credentials and custom deployment URL are not stored by this picker.</p>
	<p class="hint selected-notice" role="status" aria-live="polite">{notice}</p>
</section>

<style>
	.model-picker { min-width: 0; isolation: isolate; }
	.card-head { display: flex; align-items: center; justify-content: space-between; flex-wrap: wrap; gap: 12px; margin-bottom: 12px; }
	h3 { font-size: 15px; font-weight: 650; letter-spacing: -0.2px; margin: 0; }
	p { margin: 0 0 12px; overflow-wrap: anywhere; }
	.draft, .identity, .explanation { font-size: 13px; line-height: 1.6; }
	.hint { font-size: 12px; line-height: 1.6; color: var(--sub); font-variant-numeric: tabular-nums; }
	fieldset { border: 0; padding: 0; margin: 16px 0; min-width: 0; }
	.two { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); gap: 16px; }
	.field { display: flex; flex-direction: column; gap: 8px; min-width: 0; margin-bottom: 12px; }
	label { font-size: 13px; font-weight: 550; }
	input, select { width: 100%; min-width: 0; min-height: 44px; padding: 8px 12px; color: var(--ink); background: var(--paper); border: 1px solid var(--rule-strong); border-radius: 10px; font-size: 14px; line-height: 1.5; }
	input:hover:not(:disabled), select:hover:not(:disabled) { border-color: var(--accent); }
	input:active:not(:disabled), select:active:not(:disabled) { border-color: var(--accent-strong); }
	input:disabled, select:disabled { background: var(--rule); color: var(--sub); cursor: not-allowed; }
	:is(input, select, button, a):focus-visible { outline: 2px solid var(--accent); outline-offset: 3px; }
	.capabilities { border-top: 1px solid var(--rule-strong); border-bottom: 1px solid var(--rule-strong); padding: 12px 0 0; margin-bottom: 12px; }
	dl { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 12px 16px; margin: 0 0 12px; font-size: 12px; line-height: 1.6; }
	dt { color: var(--sub); }
	dd { margin: 0; font-weight: 550; font-variant-numeric: tabular-nums; overflow-wrap: anywhere; }
	code { font-size: 12px; background: var(--chip-bg); padding: 0 4px; border-radius: 4px; overflow-wrap: anywhere; }
	.actions { display: flex; flex-wrap: wrap; gap: 8px; }
	.a-btn { min-height: 44px; white-space: normal; }
	.a-btn.primary { color: var(--paper); box-shadow: none; }
	.a-btn:active:not(:disabled) { transform: translateY(1px); }
	a { text-decoration: underline; text-underline-offset: 3px; }
	a:hover, a:active { color: var(--accent-strong); }
	.privacy { border-top: 1px solid var(--rule); padding-top: 12px; }
	.selected-notice:empty { display: none; }
	.sr-only { position: absolute; width: 1px; height: 1px; padding: 0; margin: -1px; overflow: hidden; clip: rect(0, 0, 0, 0); white-space: nowrap; border: 0; }
	@media (max-width: 600px) { .two { grid-template-columns: minmax(0, 1fr); gap: 0; } dl { grid-template-columns: repeat(2, minmax(0, 1fr)); } .actions .a-btn { flex: 1 1 160px; } }
</style>
