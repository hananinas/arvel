<script lang="ts">
	import { onMount, tick } from 'svelte';
	import {
		getBuddySettings, saveBuddySettings, validateSettingsPatch, utf8Bytes,
		BuddySettingsRequestError, type BuddySettings, type BuddySettingsPatch,
		codexVoices, type TtsSettings, type TtsPatch, type ProviderPatch, type SearchSettings, type SearchPatch, type SettingsIssue,
		type ConversationMode, type LivePatch
	} from '$lib/buddySettings';
	import { arvelScenes } from '$lib/arvelScenes';
	import { PreferenceSaver } from '$lib/preferenceSaver.svelte';
	import { timezones, browserTimezone, timezoneLabel } from '$lib/timezones';
	import ArvelScene from '$lib/components/ArvelScene.svelte';
	import PreferenceRows, { settingsTabs, type SettingsTab } from '$lib/components/PreferenceRows.svelte';
	import OwnerAccess from '$lib/components/OwnerAccess.svelte';
	import CodexConnect from '$lib/components/CodexConnect.svelte';
	import ModelPicker from '$lib/components/ModelPicker.svelte';
	import type { CatalogSelection } from '$lib/modelCatalog';
	import { deviceRequest, isAgentSnapshot, type AgentSnapshot } from '$lib/agent';

	type ProviderDraft = { endpoint: string; model: string; apiKey: string; clearApiKey: boolean };
	type ProfileDraft = { name: string; userName: string; instructions: string };
	type TtsDraft = TtsSettings;
	type SearchKeyDraft = { apiKey: string; clearApiKey: boolean };
	type LiveDraft = { provider: string; model: string; protocol: 'nemotron_labs_realtime'; endpoint: string; apiKey: string; clearApiKey: boolean };
	let conversationMode = $state<ConversationMode>('text_pipeline');
	let chatProvider = $state('ollama');
	let live = $state<LiveDraft>({ provider: 'nebius', model: 'nvidia/NVIDIA-NemotronLabs-VoiceChat-11B', protocol: 'nemotron_labs_realtime', endpoint: '', apiKey: '', clearApiKey: false });
	let saved = $state<BuddySettings | null>(null);
	let providers = $state<Record<'chat' | 'voice', ProviderDraft>>({
		chat: { endpoint: '', model: '', apiKey: '', clearApiKey: false },
		voice: { endpoint: '', model: '', apiKey: '', clearApiKey: false }
	});
	let tts = $state<TtsDraft>({
		backend: 'local', voice: 'sol'
	});
	let search = $state<Pick<SearchSettings, 'provider' | 'enabled'>>({ provider: 'tavily', enabled: false });
	let searchKeys = $state<Record<'tavily' | 'parallel', SearchKeyDraft>>({
		tavily: { apiKey: '', clearApiKey: false }, parallel: { apiKey: '', clearApiKey: false }
	});
	let profile = $state<ProfileDraft>({ name: '', userName: '', instructions: '' });
	let runtime = $state<AgentSnapshot | null>(null);
	let statusError = $state('');
	let statusPending = $state(false);
	let ownerOperation = $state(false);
	let statusController: AbortController | null = null;
	let pending = $state<'load' | 'status' | 'save' | null>(null);
	let error = $state('');
	let issues = $state<SettingsIssue[]>([]);
	let notice = $state('');
	let checkedAt = $state('');
	let tab = $state<SettingsTab>('voice');
	let errorElement = $state<HTMLDivElement>();
	let mounted = false;
	let controller: AbortController | null = null;
	let authenticated = false;
	let settingsReady = $state(false);
	let accessGeneration = 0;
	const groups = ['chat', 'voice'] as const;
	const searchProviders = [
		{ id: 'tavily', name: 'Tavily', key: 'tavilyApiKey', clear: 'clearTavilyApiKey', configured: 'tavilyApiKeyConfigured', website: 'https://www.tavily.com/' },
		{ id: 'parallel', name: 'Parallel.ai', key: 'parallelApiKey', clear: 'clearParallelApiKey', configured: 'parallelApiKeyConfigured', website: 'https://parallel.ai/' }
	] as const;
	const saver = new PreferenceSaver((settings) => {
		if (!saved) return;
		saved = { ...saved, preferences: settings.preferences, buddy: { ...saved.buddy, readAloud: settings.buddy.readAloud, scene: settings.buddy.scene }, busy: settings.busy };
		checkedAt = new Date().toLocaleTimeString();
	});
	const suggestedZone = browserTimezone();
	let dirty = $derived(!!saved && (groups.some((group) =>
		providers[group].endpoint !== saved![group].endpoint || providers[group].model !== saved![group].model ||
		providers[group].apiKey !== '' || providers[group].clearApiKey) ||
		tts.backend !== saved.tts.backend || tts.voice !== saved.tts.voice ||
		conversationMode !== saved.conversationMode || chatProvider !== saved.chat.provider ||
		live.provider !== saved.live.provider || live.model !== saved.live.model || live.protocol !== saved.live.protocol ||
		live.endpoint !== saved.live.endpoint || live.apiKey !== '' || live.clearApiKey ||
		search.provider !== saved.search.provider || search.enabled !== saved.search.enabled ||
		searchProviders.some((provider) => searchKeys[provider.id].apiKey !== '' || searchKeys[provider.id].clearApiKey) ||
		profile.name !== saved.buddy.name || profile.instructions !== saved.buddy.instructions || profile.userName !== saved.preferences.userName));

	function replaceDraft(settings: BuddySettings) {
		for (const group of groups) {
			providers[group] = { endpoint: settings[group].endpoint, model: settings[group].model, apiKey: '', clearApiKey: false };
		}
		tts = { backend: settings.tts.backend, voice: settings.tts.voice };
		conversationMode = settings.conversationMode;
		chatProvider = settings.chat.provider;
		live = { provider: settings.live.provider, model: settings.live.model, protocol: settings.live.protocol, endpoint: settings.live.endpoint, apiKey: '', clearApiKey: false };
		search = { provider: settings.search.provider, enabled: settings.search.enabled };
		for (const provider of searchProviders) searchKeys[provider.id] = { apiKey: '', clearApiKey: false };
		profile = { name: settings.buddy.name, userName: settings.preferences.userName, instructions: settings.buddy.instructions };
	}

	function makePatch(): BuddySettingsPatch {
		if (!saved) return {};
		const patch: BuddySettingsPatch = {};
		for (const group of groups) {
			const draft = providers[group];
			const provider: ProviderPatch = {};
			if (draft.endpoint !== saved[group].endpoint) provider.endpoint = draft.endpoint;
			if (draft.model !== saved[group].model) provider.model = draft.model;
			if (draft.apiKey !== '') provider.apiKey = draft.apiKey;
			if (draft.clearApiKey) provider.clearApiKey = true;
			if (Object.keys(provider).length) patch[group] = provider;
		}
		if (chatProvider !== saved.chat.provider) patch.chat = { ...patch.chat, provider: chatProvider };
		if (conversationMode !== saved.conversationMode) patch.conversationMode = conversationMode;
		const deployment: LivePatch = {};
		if (live.provider !== saved.live.provider) deployment.provider = live.provider;
		if (live.model !== saved.live.model) deployment.model = live.model;
		if (live.protocol !== saved.live.protocol) deployment.protocol = live.protocol;
		if (live.endpoint !== saved.live.endpoint) deployment.endpoint = live.endpoint;
		if (live.apiKey) deployment.apiKey = live.apiKey;
		if (live.clearApiKey) deployment.clearApiKey = true;
		if (Object.keys(deployment).length) patch.live = deployment;
		const speech: TtsPatch = {};
		if (tts.backend !== saved.tts.backend) speech.backend = tts.backend;
		if (tts.voice !== saved.tts.voice) speech.voice = tts.voice;
		if (Object.keys(speech).length) patch.tts = speech;
		const web: SearchPatch = {};
		if (search.provider !== saved.search.provider) web.provider = search.provider;
		if (search.enabled !== saved.search.enabled) web.enabled = search.enabled;
		for (const provider of searchProviders) {
			const draft = searchKeys[provider.id];
			if (draft.apiKey !== '') web[provider.key] = draft.apiKey;
			if (draft.clearApiKey) web[provider.clear] = true;
		}
		if (Object.keys(web).length) patch.search = web;
		const changes: Partial<BuddySettings['buddy']> = {};
		if (profile.name !== saved.buddy.name) changes.name = profile.name;
		if (profile.instructions !== saved.buddy.instructions) changes.instructions = profile.instructions;
		if (Object.keys(changes).length) patch.buddy = changes;
		if (profile.userName !== saved.preferences.userName) patch.preferences = { userName: profile.userName };
		return patch;
	}

	async function focusError() {
		await tick();
		if (mounted) errorElement?.focus();
	}

	function clearSecrets() {
		for (const group of groups) providers[group].apiKey = '';
		for (const provider of searchProviders) searchKeys[provider.id].apiKey = '';
		live.apiKey = '';
	}

	function lockSettings() {
		authenticated = false;
		settingsReady = false;
		accessGeneration++;
		controller?.abort();
		controller = null;
		statusController?.abort(); statusController = null;
		runtime = null; statusError = ''; statusPending = false; ownerOperation = false;
		pending = null;
		clearSecrets();
		saver.pause();
		error = '';
		issues = [];
		notice = '';
	}

	function unlockSettings() {
		authenticated = true;
		settingsReady = false;
		accessGeneration++;
		void loadSettings();
	}

	async function loadSettings(replace = false) {
		if (!authenticated || (pending && mounted)) return;
		if (replace && dirty && !window.confirm('Reload the device’s saved settings? This discards unsaved changes and entered API keys on this page.')) return;
		pending = !saved || replace ? 'load' : 'status';
		error = '';
		issues = [];
		notice = '';
		controller = new AbortController();
		const generation = accessGeneration;
		try {
			const settings = await getBuddySettings(controller.signal);
			if (!mounted || !authenticated || generation !== accessGeneration) return;
			if (replace || !saved) replaceDraft(settings);
			else {
				for (const group of groups) {
					if (providers[group].endpoint === saved[group].endpoint) {
						if (providers[group].endpoint !== settings[group].endpoint) providers[group].apiKey = '';
						providers[group].endpoint = settings[group].endpoint;
					}
					if (providers[group].model === saved[group].model) providers[group].model = settings[group].model;
				}
				if (tts.backend === saved.tts.backend) tts.backend = settings.tts.backend;
				if (tts.voice === saved.tts.voice) tts.voice = settings.tts.voice;
				if (conversationMode === saved.conversationMode) conversationMode = settings.conversationMode;
				if (chatProvider === saved.chat.provider) {
					if (chatProvider !== settings.chat.provider) providers.chat.apiKey = '';
					chatProvider = settings.chat.provider;
				}
				for (const field of ['provider', 'model', 'protocol', 'endpoint'] as const) {
					if (live[field] === saved.live[field]) {
						if ((field === 'provider' || field === 'endpoint') && live[field] !== settings.live[field]) live.apiKey = '';
						live = { ...live, [field]: settings.live[field] };
					}
				}
				if (search.provider === saved.search.provider) search.provider = settings.search.provider;
				if (search.enabled === saved.search.enabled) search.enabled = settings.search.enabled;
				if (profile.name === saved.buddy.name) profile.name = settings.buddy.name;
				if (profile.instructions === saved.buddy.instructions) profile.instructions = settings.buddy.instructions;
				if (profile.userName === saved.preferences.userName) profile.userName = settings.preferences.userName;
			}
			saved = settings;
			settingsReady = true;
			saver.sync(settings);
			saver.resume();
			checkedAt = new Date().toLocaleTimeString();
			void refreshRuntime();
			if (replace) notice = 'Saved settings reloaded. Unsaved changes and entered keys were cleared.';
			else if (pending === 'status') notice = 'Device status refreshed. Your unsaved changes were kept.';
		} catch (failure) {
			if (!mounted || !authenticated || generation !== accessGeneration) return;
			error = failure instanceof BuddySettingsRequestError ? failure.message : 'Settings could not be loaded. Check your connection and retry. Your draft has been kept.';
			await focusError();
		} finally {
			if (generation === accessGeneration) {
				controller = null;
				if (mounted) pending = null;
			}
		}
	}

	async function saveSettings(event: SubmitEvent) {
		event.preventDefault();
		if (!authenticated || !saved || pending || saved.busy || ownerOperation || !dirty) return;
		error = '';
		notice = '';
		const patch = makePatch();
		issues = validateSettingsPatch(patch, saved);
		if (issues.length) {
			error = 'Please correct these settings before saving. Nothing has been sent to the device.';
			await focusError();
			return;
		}
		pending = 'save';
		controller = new AbortController();
		const generation = accessGeneration;
		try {
			const settings = await saveBuddySettings(patch, controller.signal);
			if (!mounted || !authenticated || generation !== accessGeneration) return;
			saved = settings;
			replaceDraft(settings);
			saver.sync(settings);
			checkedAt = new Date().toLocaleTimeString();
			void refreshRuntime();
			notice = 'Saved on Arvel. Entered API keys were cleared. Transport changes end the old voice session; the microphone stays off until you explicitly start it.';
		} catch (failure) {
			if (!mounted || !authenticated || generation !== accessGeneration) return;
			if (failure instanceof BuddySettingsRequestError && failure.status === 409 && saved) saved = { ...saved, busy: true };
			error = failure instanceof BuddySettingsRequestError ? failure.message : 'The save could not be confirmed. Your draft has been kept. Reload saved values to check whether the device saved them.';
			error += ' Submitted keys were erased from this form; re-enter them before retrying.';
			await focusError();
		} finally {
			for (const group of groups) {
				if (generation === accessGeneration) providers[group].apiKey = '';
				if (patch[group]) delete patch[group]!.apiKey;
			}
			if (generation === accessGeneration) live.apiKey = '';
			if (patch.live) delete patch.live.apiKey;
			for (const provider of searchProviders) {
				if (generation === accessGeneration) searchKeys[provider.id].apiKey = '';
				if (patch.search) delete patch.search[provider.key];
			}
			if (generation === accessGeneration) {
				controller = null;
				if (mounted) pending = null;
			}
		}
	}

	function usePreset(group: 'chat' | 'voice', endpoint: string, model: string, provider?: string) {
		if (providers[group].endpoint !== endpoint) providers[group].apiKey = '';
		providers[group].endpoint = endpoint;
		providers[group].model = model;
		if (group === 'chat' && provider) chatProvider = provider;
		notice = 'Preset added to your draft only. Review the endpoint, model and API key before saving.';
	}

	function selectConversation(selection: CatalogSelection) {
		conversationMode = selection.kind;
		if (selection.kind === 'native_audio') {
			if (selection.provider !== live.provider || selection.endpoint && selection.endpoint !== live.endpoint) live.apiKey = '';
			live.provider = selection.provider;
			live.model = selection.model;
			if (selection.endpoint) live.endpoint = selection.endpoint;
			live.protocol = 'nemotron_labs_realtime';
		} else if (selection.kind === 'text_pipeline') {
			if (selection.provider !== chatProvider || selection.endpoint && selection.endpoint !== providers.chat.endpoint) providers.chat.apiKey = '';
			chatProvider = selection.provider;
			if (selection.model) providers.chat.model = selection.model;
			if (selection.endpoint) providers.chat.endpoint = selection.endpoint;
		}
		notice = 'Conversation selection added to the draft only. Review its endpoint and credentials, then Save. No microphone was opened.';
	}

	function toggleClear(group: 'chat' | 'voice', event: Event) {
		const checked = (event.currentTarget as HTMLInputElement).checked;
		providers[group].clearApiKey = checked;
		if (checked) providers[group].apiKey = '';
	}

	function toggleSearchClear(provider: 'tavily' | 'parallel', event: Event) {
		searchKeys[provider].clearApiKey = (event.currentTarget as HTMLInputElement).checked;
		if (searchKeys[provider].clearApiKey) searchKeys[provider].apiKey = '';
	}

	async function refreshRuntime() {
		if (!mounted || !authenticated || statusPending || ownerOperation) return;
		statusPending = true;
		statusController = new AbortController();
		const generation = accessGeneration;
		try {
			const value = await deviceRequest<unknown>('/rest/device', { signal: statusController.signal });
			if (!mounted || !authenticated || generation !== accessGeneration) return;
			if (!isAgentSnapshot(value)) throw new Error('unexpected projection');
			runtime = value;
			statusError = '';
		} catch {
			if (mounted && authenticated && generation === accessGeneration) statusError = 'Device status unavailable. Displayed observations may be stale; refresh before acting.';
		} finally {
			if (generation === accessGeneration) { statusPending = false; statusController = null; }
		}
	}

	async function checkReadiness() {
		if (!saved || dirty || pending || ownerOperation || statusPending || saved.conversationMode !== 'native_audio') return;
		ownerOperation = true; statusError = '';
		statusController?.abort();
		statusController = new AbortController();
		const generation = accessGeneration;
		try {
			const value = await deviceRequest<{ accepted?: boolean }>('/rest/agent/readiness', {
				method: 'POST', body: '{}', signal: statusController.signal
			});
			if (!value || value.accepted !== true) throw new Error('not accepted');
			if (mounted && authenticated && generation === accessGeneration)
				notice = 'Saved deployment health check accepted. No microphone opened. Refresh status for the authenticated health result; speech and GPU inference remain unexercised.';
		} catch {
			if (mounted && authenticated && generation === accessGeneration) statusError = 'The saved deployment health check could not be accepted. Check owner login, network and device status.';
		} finally {
			if (generation === accessGeneration) { ownerOperation = false; statusController = null; void refreshRuntime(); }
		}
	}

	function invalid(field: string) {
		return issues.some((issue) => issue.field === field);
	}

	onMount(() => {
		pending = null;
		mounted = true;
		return () => {
			mounted = false;
			saver.pause();
			controller?.abort();
			statusController?.abort();
			clearSecrets();
		};
	});
</script>

<svelte:head>
	<title>Settings · Arvel</title>
	<meta name="description" content="Arvel’s voice, display, personality and API settings, saved on your device." />
</svelte:head>

<section class="page" aria-labelledby="settings-title">
	<header class="page-head">
		<div>
			<h1 id="settings-title">Settings</h1>
			<p class="lead">Log in to edit Arvel on your home Wi-Fi. Creating or recovering web access needs a code from Settings &gt; Device &gt; Web login on Arvel’s screen.</p>
		</div>
	</header>

	<OwnerAccess onauthenticated={unlockSettings} onlocked={lockSettings} onclearsecrets={clearSecrets}>
		<p class="device-state" role="status" aria-live="polite">
			<span class="dot" class:busy={saved?.busy} class:off={!saved}></span>
			{#if pending === 'load'}Loading from Arvel…{:else if pending === 'save'}Saving on Arvel…{:else if pending === 'status'}Checking Arvel…{:else if saved}{saved.busy ? 'Arvel is busy' : 'Arvel is idle'} · checked {checkedAt}{:else}Not loaded{/if}
		</p>

	{#if error}
		<div class="notice error" role="alert" tabindex="-1" bind:this={errorElement}>
			<strong>{error}</strong>
			{#if issues.length}
				<ul>{#each issues as issue}<li><a href={`#${issue.field}`}>{issue.message}</a></li>{/each}</ul>
			{/if}
		</div>
	{/if}
	{#if saver.error}<div class="notice error" role="alert">{saver.error}</div>{/if}
	<p class="success" role="status" aria-live="polite">{notice}</p>
	{#if statusError}<p class="notice error" role="alert">{statusError}</p>{/if}

	{#if !saved || !saver.values || !settingsReady}
		<button class="a-btn primary" type="button" disabled={!!pending} onclick={() => loadSettings()}>{pending ? 'Loading…' : 'Retry loading settings'}</button>
	{:else}
		{@const values = saver.values}
		<div class="grid">
			<section class="panel-card prefs" aria-labelledby="prefs-title">
				<div class="tab-list" role="tablist" aria-label="Device settings">
					<h2 id="prefs-title" class="section-title tabs-title">On the device</h2>
					{#each settingsTabs as entry (entry.id)}
						<button type="button" role="tab" aria-selected={tab === entry.id} class="tab-btn" class:on={tab === entry.id} onclick={() => (tab = entry.id)}>{entry.name}</button>
					{/each}
				</div>
				<div class="vrule"></div>
				<div class="rows-wrap" role="tabpanel" aria-label={settingsTabs.find((entry) => entry.id === tab)?.name}>
					<PreferenceRows {tab} {values} conversationMode={saved.conversationMode}
						model={saved.conversationMode === 'native_audio' ? saved.live.model : saved.conversationMode === 'codex_live' ? 'gpt-live-1-codex' : saved.chat.model}
						ssid="Shown on Arvel’s screen" firmware="Shown on Arvel’s screen"
						disabled={saved.busy}
						onchange={(key, value, delay) => saver.set(key, value, delay)}
						onscene={() => document.getElementById('buddy-scene')?.scrollIntoView({ behavior: 'smooth', block: 'center' })} />
					<p class="hint">Available changes apply straight away, just like on Arvel’s screen. Voice style and speaking speed apply only to Local speech. Start the microphone explicitly; background wake-word audio is not uploaded. Action approval is unavailable.</p>
				</div>
			</section>

			<section class="panel-card" id="buddy-scene" aria-labelledby="scene-title">
				<div class="card-head">
					<h2 id="scene-title" class="section-title">Where should {saved.buddy.name || 'Arvel'} hang out?</h2>
					<span class="hint">Saved as soon as you pick one</span>
				</div>
				<div class="scene-grid" role="radiogroup" aria-labelledby="scene-title">
					{#each arvelScenes as option (option.id)}
						<button type="button" role="radio" aria-checked={values.scene === option.id} class="scene-pick" disabled={saved.busy} onclick={() => saver.set('scene', option.id)}>
							<span class="thumb" class:on={values.scene === option.id}><ArvelScene scene={option.id} align="xMaxYMid slice" /></span>
							<span class="scene-name" class:on={values.scene === option.id}>{option.name}</span>
						</button>
					{/each}
				</div>
			</section>

			<section class="panel-card" aria-labelledby="tz-title">
				<div class="card-head">
					<h2 id="tz-title" class="section-title">Time zone</h2>
					<span class="hint">For the clock, greetings and dimming at night</span>
				</div>
				<div class="tz-row">
					<label class="sr-only" for="pref-timezone">Time zone</label>
					<select id="pref-timezone" value={values.timezone} disabled={saved.busy} onchange={(event) => saver.set('timezone', event.currentTarget.value)}>
						{#if !timezones.some((entry) => entry.tz === values.timezone)}<option value={values.timezone}>{values.timezone}</option>{/if}
						{#each timezones as entry (entry.tz)}<option value={entry.tz}>{entry.label}</option>{/each}
					</select>
					{#if suggestedZone && suggestedZone !== values.timezone}
						<button type="button" class="a-btn ghost" disabled={saved.busy} onclick={() => saver.set('timezone', suggestedZone)}>Use this browser’s zone ({timezoneLabel(suggestedZone)})</button>
					{/if}
				</div>
			</section>
		</div>

		<form onsubmit={saveSettings} novalidate aria-label="Arvel profile and API settings" aria-busy={pending === 'save'}>
			<fieldset class="panel-card section" disabled={!!pending}>
				<legend class="section-title">Arvel</legend>
				<div class="two">
					<div class="field">
						<label for="buddy-name">Buddy name</label>
						<input id="buddy-name" type="text" bind:value={profile.name} aria-invalid={invalid('buddy-name')} aria-describedby="buddy-name-help" autocomplete="off" />
						<p id="buddy-name-help" class="hint">{utf8Bytes(profile.name)} / 32 bytes. Shown on the screen and used in replies.</p>
					</div>
					<div class="field">
						<label for="pref-userName">Your name</label>
						<input id="pref-userName" type="text" bind:value={profile.userName} aria-invalid={invalid('pref-userName')} aria-describedby="user-name-help" autocomplete="given-name" />
						<p id="user-name-help" class="hint">{utf8Bytes(profile.userName)} / 32 bytes. Used for “Morning, {profile.userName.trim() || 'you'}.” Optional.</p>
					</div>
				</div>
				<div class="field">
					<label for="buddy-instructions">How Arvel should respond</label>
					<textarea id="buddy-instructions" rows="5" bind:value={profile.instructions} aria-invalid={invalid('buddy-instructions')} aria-describedby="buddy-instructions-help"></textarea>
					<p id="buddy-instructions-help" class="hint">{utf8Bytes(profile.instructions)} / 2,048 bytes. Tone, length or helpful habits. Personality (Calm, Chatty, Focused) is set above.</p>
				</div>
			</fieldset>

			<fieldset class="panel-card section" id="conversation-model" disabled={!!pending}>
				<legend class="section-title">Conversation model &amp; deployment</legend>
				<div class="field">
					<label for="conversation-mode">Conversation transport</label>
					<select id="conversation-mode" bind:value={conversationMode} aria-invalid={invalid('conversation-mode')} aria-describedby="conversation-mode-help">
						<option value="text_pipeline">Text model · explicit STT/text/TTS pipeline</option>
						<option value="codex_live">Codex subscription · direct live voice</option>
						<option value="native_audio">NVIDIA Labs 11B · native speech</option>
					</select>
					<p id="conversation-mode-help" class="hint">One selected transport. No automatic model or paid speech fallback. Standalone read-aloud is a separate saved profile.</p>
				</div>
				<details class="model-browser">
					<summary>Browse models.dev or choose custom/self-hosted</summary>
				<ModelPicker providerId={conversationMode === 'native_audio' ? live.provider : conversationMode === 'codex_live' ? 'openai' : chatProvider}
					modelId={conversationMode === 'native_audio' ? live.model : conversationMode === 'codex_live' ? 'gpt-live-1-codex' : providers.chat.model}
					disabled={!!pending} onselect={selectConversation} />
				</details>
				{#if conversationMode === 'native_audio'}
					<p class="lead small"><strong>Speech provided by this model.</strong> This is the released Labs 11B self-hosted deployment, not Nebius Token Factory text inference or the early-access 12B demo. No STT, separate text reasoning model or TTS credential is required.</p>
					<div class="two">
						<div class="field">
							<label for="live-provider">Hosting provider identity</label>
							<input id="live-provider" bind:value={live.provider} oninput={() => { live.apiKey = ''; }} aria-invalid={invalid('live-provider')} aria-describedby="live-provider-help" autocomplete="off" />
							<p id="live-provider-help" class="hint">For example, your Nebius AI Cloud deployment. This is owner-configured provenance, not an official hosted catalog listing.</p>
						</div>
						<div class="field">
							<label for="live-model">Released model</label>
							<input id="live-model" value={live.model} readonly aria-describedby="live-protocol" />
							<p id="live-protocol" class="hint">Implemented protocol: NemotronLabs realtime PCM WebSocket. Native typed conversation and arbitrary read-aloud are unsupported.</p>
						</div>
					</div>
					<div class="field">
						<label for="live-endpoint">Native deployment WebSocket endpoint</label>
						<input id="live-endpoint" type="url" bind:value={live.endpoint} oninput={() => { live.apiKey = ''; }} aria-invalid={invalid('live-endpoint')} aria-describedby="live-endpoint-help live-key-rule" autocomplete="off" autocapitalize="off" spellcheck="false" />
						<p id="live-endpoint-help" class="hint">{utf8Bytes(live.endpoint)} / 512 bytes. Exact wss://host[:port]/v1/realtime; verified certificate/hostname and bearer authentication. No redirects, credentials in URLs, query or fragment.</p>
					</div>
					<p id="live-key-rule" class="endpoint-rule">Changing the provider or endpoint requires a newly entered key or explicit clearing. Entered keys are discarded when either field changes.</p>
					<div class="field">
						<label for="live-key">Deployment bearer key</label>
						<p class="key-state">{saved.live.apiKeyConfigured ? 'A key is saved on Arvel (hidden).' : 'No key saved yet.'}</p>
						<input id="live-key" type="password" bind:value={live.apiKey} disabled={live.clearApiKey} aria-invalid={invalid('live-key')} aria-describedby="live-key-rule" autocomplete="new-password" autocapitalize="off" spellcheck="false" />
					</div>
					<label class="check" for="live-clear-key">
						<input id="live-clear-key" type="checkbox" bind:checked={live.clearApiKey} onchange={() => { live.apiKey = ''; }} />
						<span>Clear the saved deployment key when saving</span>
					</label>
					<p class="hint">Configured is not authenticated or exercised. Follow the native deployment instructions and test readiness without opening the microphone. Selected audio and context are sent to the configured cloud endpoint; this is not end-to-end private hardware processing.</p>
					<div class="readiness" aria-live="polite">
						<p><strong>Saved deployment health:</strong> {runtime?.nativeReadiness.state ?? 'untested'}{dirty ? ' · draft not checked' : ''}</p>
						{#if runtime?.nativeReadiness.error}<p class="hint">{runtime.nativeReadiness.error}</p>{/if}
						{#if runtime?.nativeReadiness.state === 'ready'}<p class="hint">Authenticated health endpoint ready. This does not establish that GPU inference, speech or tools have been exercised.</p>{/if}
						<div class="actions">
							<button type="button" class="a-btn ghost" disabled={dirty || !!pending || ownerOperation || statusPending || saved.busy || !saved.live.apiKeyConfigured} onclick={checkReadiness}>{ownerOperation ? 'Requesting health check…' : 'Check saved deployment · no microphone'}</button>
							<button type="button" class="a-btn ghost" disabled={statusPending || ownerOperation} onclick={refreshRuntime}>{statusPending ? 'Refreshing…' : 'Refresh health status'}</button>
						</div>
						<p class="hint">Save this profile first. This checks only the saved authenticated deployment and never starts capture.</p>
					</div>
				{/if}
			</fieldset>

			{#if conversationMode !== 'native_audio'}

			<fieldset class="panel-card section" id="speech-output" disabled={!!pending}>
				<legend class="section-title">Standalone read-aloud &amp; Codex account</legend>
				<p class="lead small">Read-aloud handles typed text independently of the selected conversation transport. Codex live uses its own microphone/speaker connection and saved account; Local read-aloud uses eSpeak on Arvel.</p>
				<div class="two">
					<div class="field">
						<label for="tts-backend">Standalone read-aloud backend</label>
						<select id="tts-backend" bind:value={tts.backend} aria-invalid={invalid('tts-backend')} aria-describedby="tts-backend-help">
							<option value="local">Local speech</option>
							<option value="codex">Codex subscription · direct</option>
						</select>
						<p id="tts-backend-help" class="hint">This choice does not change conversation mode. Switching conversation transport above retains the saved text, transcription, read-aloud and Codex account profiles.</p>
					</div>
					<div class="field">
						<label for="tts-voice">Codex voice</label>
						<select id="tts-voice" bind:value={tts.voice} aria-invalid={invalid('tts-voice')} aria-describedby="tts-voice-help">
							{#each codexVoices as voice (voice)}<option value={voice}>{voice[0].toUpperCase() + voice.slice(1)}</option>{/each}
						</select>
						<p id="tts-voice-help" class="hint">Used only by Codex. Local voice style and speaking speed are set above and do not affect Codex.</p>
					</div>
				</div>
				<CodexConnect />
				<p class="hint lan"><strong>Direct encrypted voice.</strong> Arvel connects to OpenAI using verified HTTPS and encrypted WebRTC audio. No bridge endpoint or API key is needed for ordinary Codex conversation. Device-action delegations still use the configured chat backend below.</p>
			</fieldset>
			{/if}

			<fieldset class="panel-card section" id="chat-provider" disabled={!!pending}>
				<legend class="section-title">Chat API &amp; model</legend>
				<p class="lead small">Saved text profile for typed chat, text-pipeline voice and Codex delegations. Native structured tool calls do not require this second model.</p>
				<div class="field">
					<label for="chat-provider-id">Text provider identity</label>
					<input id="chat-provider-id" bind:value={chatProvider} oninput={() => { providers.chat.apiKey = ''; }} aria-invalid={invalid('chat-provider-id')} aria-describedby="chat-provider-id-help" autocomplete="off" />
					<p id="chat-provider-id-help" class="hint">Provider-scoped model identity; up to 64 UTF-8 bytes. Custom endpoints use the implemented OpenAI-compatible text adapter, not a provider SDK.</p>
				</div>
				<div class="presets" aria-label="Chat endpoint presets">
					<button class="a-btn ghost" type="button" onclick={() => usePreset('chat', 'https://api.tokenfactory.us-north1.nebius.com/v1/chat/completions', 'deepseek-ai/DeepSeek-V4.1-Flash', 'nebius')}>Nebius · DeepSeek V4.1 Flash</button>
					<button class="a-btn ghost" type="button" onclick={() => usePreset('chat', 'https://ollama.com/v1/chat/completions', 'glm-5.3', 'ollama')}>Ollama · glm-5.3</button>
					<button class="a-btn ghost" type="button" onclick={() => usePreset('chat', 'https://api.openai.com/v1/chat/completions', 'gpt-4.1-mini', 'openai')}>OpenAI · gpt-4.1-mini</button>
				</div>
				{@render providerFields('chat')}
				<p class="hint">For ollama.com chat only, a model’s trailing <code>:cloud</code> is removed when saving.</p>
			</fieldset>

			{#if conversationMode === 'text_pipeline'}
			<fieldset class="panel-card section" id="voice-provider" disabled={!!pending}>
				<legend class="section-title">Speech-to-text API · text pipeline only</legend>
				<p class="lead small">Explicit transcription for a selected text-model voice pipeline. Codex live and native audio never use this credential or fall back to it.</p>
				<div class="presets">
					<button class="a-btn ghost" type="button" onclick={() => usePreset('voice', 'https://api.openai.com/v1/audio/transcriptions', 'whisper-1')}>OpenAI · whisper-1</button>
				</div>
				{@render providerFields('voice')}
			</fieldset>
			{/if}

			<fieldset class="panel-card section" id="search-tools" disabled={!!pending}>
				<legend class="section-title">Search tools</legend>
				<p class="lead small">Choose the service Arvel uses for web search. Each provider keeps its own key; switching providers does not delete either saved key. Queries go to the selected service and may incur charges.</p>
				<div class="field">
					<label for="search-provider">Search provider</label>
					<select id="search-provider" bind:value={search.provider} aria-invalid={invalid('search-provider')} aria-describedby="search-provider-help">
						<option value="tavily">Tavily</option>
						<option value="parallel">Parallel.ai</option>
					</select>
					<p id="search-provider-help" class="hint">Only the selected provider’s key is used. Search has fixed verified HTTPS endpoints; no endpoint URL is needed.</p>
				</div>
				<label class="check" for="search-enabled">
					<input id="search-enabled" type="checkbox" bind:checked={search.enabled} aria-invalid={invalid('search-enabled')} aria-describedby="search-enabled-help" />
					<span>Enable web search</span>
				</label>
				<p id="search-enabled-help" class="hint">Save a key for the selected provider before searching. Turning search off keeps both saved keys.</p>
				<div class="two">
					{#each searchProviders as provider (provider.id)}
						<div>
							<div class="field">
								<label for={`search-${provider.id}-key`}>{provider.name} API key</label>
								<p class="key-state" id={`search-${provider.id}-key-state`}><span class="dot" class:off={!saved.search[provider.configured]}></span>{saved.search[provider.configured] ? 'A key is saved on the device (hidden).' : 'No key saved yet.'}</p>
								<input id={`search-${provider.id}-key`} type="password" bind:value={searchKeys[provider.id].apiKey} disabled={searchKeys[provider.id].clearApiKey} aria-invalid={invalid(`search-${provider.id}-key`)} aria-describedby={`search-${provider.id}-key-state search-${provider.id}-key-help`} autocomplete="new-password" autocapitalize="off" spellcheck="false" />
								<p id={`search-${provider.id}-key-help`} class="hint">Leave empty to keep the saved key. Up to 1,024 ASCII bytes, without spaces. <a href={provider.website} target="_blank" rel="noopener noreferrer">Get a {provider.name} key (opens a new tab)</a>.</p>
							</div>
							<label class="check" for={`search-${provider.id}-clear-key`}>
								<input id={`search-${provider.id}-clear-key`} type="checkbox" checked={searchKeys[provider.id].clearApiKey} onchange={(event) => toggleSearchClear(provider.id, event)} aria-invalid={invalid(`search-${provider.id}-clear-key`)} />
								<span>Clear the saved {provider.name} key when saving</span>
							</label>
						</div>
					{/each}
				</div>
				<p class="hint lan">Keys are stored on Arvel and never read back into this form. Submitted keys are erased here even if saving fails. Use a trusted local network: HTTP Settings does not encrypt submitted keys; flash/NVS is not encrypted.</p>
			</fieldset>
			<section class="panel-card section" aria-labelledby="memory-title">
				<h2 id="memory-title" class="section-title">Local memory</h2>
				<p class="lead small">Finalized conversation and tool evidence live on your SD card, not in an archive-sized model request. No raw microphone or generated audio is archived by default.</p>
				<p role="status"><strong>Storage:</strong> {runtime?.storage.state ?? 'Not observed'} · {runtime?.storage.durable ? 'durable archive available' : 'new persistence not confirmed'}</p>
				{#if runtime}<p class="hint">Database {runtime.storage.databaseBytes.toLocaleString()} bytes. SQLite allocation {runtime.storage.sqliteBytes.toLocaleString()} bytes (high-water {runtime.storage.sqliteHighwater.toLocaleString()}).</p>{/if}
				<p class="hint">Missing, full, removed or read-only storage does not trigger formatting. An unavailable archive cannot promise persistence.</p>
				<div class="actions">
					<a class="a-btn ghost" href="/#history">Inspect, export or delete history</a>
					<button type="button" class="a-btn ghost" disabled={statusPending || ownerOperation} onclick={refreshRuntime}>Refresh storage status</button>
				</div>
				<p class="hint lan"><strong>Privacy boundary:</strong> history is local but SD and NVS are not encrypted. The configured cloud model receives selected audio and context. LAN HTTP does not encrypt owner requests. This is not end-to-end encrypted storage. Deletion is logical, not guaranteed secure erasure on a wear-levelled card.</p>
			</section>

			<section class="panel-card section" aria-labelledby="other-tools-title">
				<h2 id="other-tools-title" class="section-title">Other desk tools</h2>
				<p class="lead small">Weather uses Open-Meteo without an API key. Timers/Pomodoro, notes/to-dos, calculations and unit conversions run locally and need no tool keys. Chat and device-action delegations use the Chat API above; Local transcription uses the speech-to-text API.</p>
			</section>

			<div class="save-bar">
				<div>
					<strong>{dirty ? 'Unsaved changes' : 'Everything is saved'}</strong>
					<p class="hint">{saved.busy ? 'Saving waits until Arvel is idle.' : 'Profile and API changes are saved when you press Save. Keys are stored on the device and never shown again.'}</p>
				</div>
				<div class="actions">
					<button class="a-btn ghost" type="button" disabled={!!pending || ownerOperation} onclick={() => loadSettings()}>Check status</button>
					<button class="a-btn ghost" type="button" disabled={!!pending || ownerOperation} onclick={() => loadSettings(true)}>Reload saved</button>
					<button class="a-btn primary" type="submit" disabled={!!pending || ownerOperation || !dirty || saved.busy}>{pending === 'save' ? 'Saving…' : 'Save'}</button>
				</div>
			</div>
			<p class="hint lan">Saved API keys are never returned to this browser. The owner login changes only access to Settings, not native microphone access, Wi-Fi onboarding or legacy chat policy.</p>
		</form>
	{/if}
	</OwnerAccess>
</section>

{#snippet providerFields(group: 'chat' | 'voice')}
	<div class="provider-grid">
		<div class="field">
			<label for={`${group}-endpoint`}>{group === 'chat' ? 'Endpoint URL' : 'Transcription endpoint URL'}</label>
			<input id={`${group}-endpoint`} type="url" bind:value={providers[group].endpoint} oninput={() => { providers[group].apiKey = ''; }} aria-invalid={invalid(`${group}-endpoint`)} aria-describedby={`${group}-endpoint-help ${group}-endpoint-rule`} autocapitalize="off" spellcheck="false" autocomplete="off" />
			<p id={`${group}-endpoint-help`} class="hint">{utf8Bytes(providers[group].endpoint)} / 512 bytes. HTTPS only; no login, query string or fragment.</p>
		</div>
		<div class="field">
			<label for={`${group}-model`}>Model</label>
			<input id={`${group}-model`} type="text" bind:value={providers[group].model} aria-invalid={invalid(`${group}-model`)} aria-describedby={`${group}-model-help`} autocapitalize="off" spellcheck="false" autocomplete="off" />
			<p id={`${group}-model-help`} class="hint">{utf8Bytes(providers[group].model)} / 128 bytes. Exact provider model name.</p>
		</div>
	</div>
	<p id={`${group}-endpoint-rule`} class="endpoint-rule" class:needed={saved && providers[group].endpoint !== saved[group].endpoint && saved[group].apiKeyConfigured}>Changing the endpoint needs a fresh API key or clearing the saved one — Arvel never forwards a saved key to a new URL.</p>
	<div class="field key">
		<label for={`${group}-key`}>API key</label>
		<p class="key-state"><span class="dot" class:off={!saved?.[group].apiKeyConfigured}></span>{saved?.[group].apiKeyConfigured ? 'A key is saved on the device (hidden).' : 'No key saved yet.'}</p>
		<input id={`${group}-key`} type="password" bind:value={providers[group].apiKey} disabled={providers[group].clearApiKey} aria-invalid={invalid(`${group}-key`)} aria-describedby={`${group}-key-help ${group}-endpoint-rule`} autocomplete="new-password" autocapitalize="off" spellcheck="false" />
		<p id={`${group}-key-help`} class="hint">Leave empty to keep the saved key. Up to 1,024 bytes, no spaces. Submitted keys are erased from this form even if saving fails.</p>
	</div>
	<label class="check" for={`${group}-clear-key`}>
		<input id={`${group}-clear-key`} type="checkbox" checked={providers[group].clearApiKey} onchange={(event) => toggleClear(group, event)} />
		<span>Clear the saved key when saving</span>
	</label>
{/snippet}

<style>
	.page { max-width: 1120px; margin: 0 auto; padding: 28px 32px; }
	.page-head { display: flex; justify-content: space-between; align-items: flex-end; gap: 20px; margin-bottom: 16px; flex-wrap: wrap; }
	h1 { font-size: 21px; font-weight: 650; letter-spacing: -0.4px; line-height: 1.2; }
	.lead { font-size: 14px; line-height: 1.35; color: var(--sub); margin-top: 4px; max-width: 62ch; }
	.lead.small { font-size: 13px; margin: 0 0 12px; }
	.device-state { display: flex; align-items: center; gap: 6px; font-size: 11px; font-weight: 700; letter-spacing: 0.6px; color: var(--sub); text-transform: uppercase; }
	.dot { width: 7px; height: 7px; border-radius: 4px; background: var(--ready); flex-shrink: 0; display: inline-block; }
	.dot.busy { background: var(--thinking); }
	.dot.off { background: var(--faint); }
	.success { font-size: 13px; color: var(--accent-strong); margin: 0 0 12px; }
	.success:empty { display: none; }
	.notice { margin-bottom: 12px; }
	.notice ul { padding-left: 20px; margin: 6px 0 0; }
	.grid { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); gap: 16px; margin-bottom: 16px; }
	.panel-card { background: var(--paper); border-radius: 16px; box-shadow: var(--shadow-panel); padding: 14px 16px; min-width: 0; }
	.section-title { font-size: 15px; font-weight: 650; letter-spacing: -0.2px; }
	.card-head { display: flex; align-items: baseline; justify-content: space-between; gap: 12px; margin-bottom: 12px; flex-wrap: wrap; }
	.prefs { grid-column: 1 / -1; display: flex; gap: 12px; padding: 10px 14px 10px 10px; }
	.tab-list { width: 132px; flex-shrink: 0; display: flex; flex-direction: column; gap: 3px; }
	.tabs-title { padding: 3px 8px 5px; }
	.tab-btn { height: 32px; border: none; border-radius: 8px; padding: 0 9px; background: transparent; color: var(--ink); font-size: 13px; font-weight: 500; text-align: left; cursor: pointer; }
	.tab-btn.on { background: var(--accent-soft); color: var(--accent-strong); font-weight: 650; }
	.tab-btn:hover:not(.on) { background: var(--chip-bg); }
	.vrule { width: 1px; background: var(--rule); flex-shrink: 0; }
	.rows-wrap { flex: 1; min-width: 0; display: flex; flex-direction: column; justify-content: center; }
	.rows-wrap :global(.row) { height: 44px; }
	.rows-wrap .hint { margin-top: 6px; }
	.scene-grid { display: grid; grid-template-columns: repeat(6, minmax(0, 1fr)); gap: 10px 8px; }
	.scene-pick { border: none; background: none; padding: 0; display: flex; flex-direction: column; align-items: center; gap: 6px; cursor: pointer; color: var(--ink); }
	.scene-pick:disabled { cursor: not-allowed; opacity: 0.6; }
	.thumb { width: 100%; max-width: 80px; aspect-ratio: 80 / 56; border-radius: 10px; overflow: hidden; isolation: isolate; box-shadow: 0 0 0 1px rgba(0, 0, 0, 0.08); }
	.thumb.on { box-shadow: 0 0 0 2px var(--paper), 0 0 0 4px var(--accent); }
	.scene-name { font-size: 12px; font-weight: 500; line-height: 1.2; }
	.scene-name.on { font-weight: 700; }
	.tz-row { display: flex; flex-direction: column; gap: 10px; align-items: flex-start; }
	select { width: 100%; min-height: 40px; border: 1px solid var(--rule-strong); border-radius: 10px; background: var(--paper); color: var(--ink); padding: 0 10px; font-size: 14px; }
	form { display: flex; flex-direction: column; gap: 16px; }
	fieldset.section { border: none; margin: 0; padding: 14px 16px 16px; }
	legend.section-title { float: left; width: 100%; margin-bottom: 10px; padding: 0; }
	legend.section-title + * { clear: both; }
	.two { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }
	.field { display: flex; flex-direction: column; gap: 6px; min-width: 0; margin-bottom: 12px; }
	.field label { font-size: 13px; font-weight: 550; }
	.field input, textarea { width: 100%; min-height: 40px; padding: 8px 12px; color: var(--ink); background: var(--paper); border: 1px solid var(--rule-strong); border-radius: 10px; font-size: 14px; line-height: 1.45; }
	#search-tools input[type='password'] { background: var(--paper); }
	#search-tools a { text-decoration: underline; text-underline-offset: 2px; }
	#search-tools a:hover { color: var(--accent-strong); }
	#search-tools :is(input, select, a):focus-visible { outline: 2px solid var(--accent); outline-offset: 3px; }
	#search-tools .check:active { color: var(--accent-strong); }
	textarea { resize: vertical; }
	.field input:hover:not(:disabled), textarea:hover:not(:disabled), select:hover:not(:disabled) { border-color: var(--accent); }
	input:disabled, textarea:disabled, select:disabled { background: var(--rule); color: var(--sub); cursor: not-allowed; }
	input[aria-invalid='true'], textarea[aria-invalid='true'], select[aria-invalid='true'] { border: 2px solid var(--mic); }
	.hint { font-size: 12px; line-height: 1.45; color: var(--sub); overflow-wrap: anywhere; font-variant-numeric: tabular-nums; }
	.presets { display: flex; flex-wrap: wrap; gap: 8px; margin-bottom: 14px; }
	.provider-grid { display: grid; grid-template-columns: minmax(0, 2fr) minmax(0, 1fr); gap: 16px; }
	.endpoint-rule { font-size: 12px; line-height: 1.45; color: var(--sub); border-left: 3px solid var(--rule-strong); padding: 4px 10px; margin: 0 0 14px; }
	.endpoint-rule.needed { border-left-color: var(--mic); color: #7a3a2c; background: #f6e6e1; }
	.key { max-width: 560px; }
	.key-state { display: flex; align-items: center; gap: 6px; font-size: 12px; color: var(--sub); }
	.check { display: flex; align-items: center; gap: 8px; min-height: 40px; font-size: 13px; cursor: pointer; }
	.check input { width: 18px; height: 18px; accent-color: var(--accent); }
	.save-bar { position: sticky; bottom: 12px; display: flex; justify-content: space-between; align-items: center; gap: 16px; flex-wrap: wrap; background: var(--paper); border-radius: 16px; box-shadow: var(--shadow-panel); padding: 12px 16px; }
	.save-bar strong { font-size: 13px; font-weight: 650; }
	.actions { display: flex; flex-wrap: wrap; gap: 8px; }
	.lan { border-left: 3px solid var(--mic); padding-left: 10px; }
	.readiness { display: grid; gap: 8px; margin-top: 16px; border-top: 1px solid var(--rule); padding-top: 16px; }
	.page :is(button, input, textarea, select, a):focus-visible { outline: 2px solid var(--accent); outline-offset: 3px; }
	.page button:active:not(:disabled) { transform: translateY(1px); }
	.page button:disabled { cursor: not-allowed; opacity: 0.6; }
	.sr-only { position: absolute; width: 1px; height: 1px; padding: 0; margin: -1px; overflow: hidden; clip: rect(0, 0, 0, 0); white-space: nowrap; border: 0; }
	code { font-size: 12px; background: var(--chip-bg); padding: 1px 4px; border-radius: 4px; }
	@media (max-width: 880px) {
		.page { padding: 20px 16px; }
		.grid { grid-template-columns: minmax(0, 1fr); }
		.scene-grid { grid-template-columns: repeat(4, minmax(0, 1fr)); }
	}
	@media (max-width: 600px) {
		.prefs { flex-direction: column; }
		.tab-list { width: auto; flex-direction: row; flex-wrap: wrap; }
		.tabs-title { width: 100%; }
		.vrule { width: auto; height: 1px; }
		.two, .provider-grid { grid-template-columns: minmax(0, 1fr); gap: 0; }
		.scene-grid { grid-template-columns: repeat(3, minmax(0, 1fr)); }
		.actions { width: 100%; }
		.actions .a-btn { flex: 1; }
	}
</style>
