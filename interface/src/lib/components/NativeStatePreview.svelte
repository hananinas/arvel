<script lang="ts">
	import type { AgentSnapshot, VoicePhase } from '$lib/agent';
	import { sceneById } from '$lib/arvelScenes';
	import ArvelScene from './ArvelScene.svelte';
	import ArvelBird from './ArvelBird.svelte';

	let { snapshot, authenticated = false, loading = false }: {
		snapshot: AgentSnapshot | null;
		authenticated?: boolean;
		loading?: boolean;
	} = $props();
	let animationsPaused = $state(false);
	let scene = $derived(sceneById(snapshot?.scene));
	let animate = $derived(!animationsPaused && !snapshot?.screenOff);
	const voiceLabels: Record<VoicePhase, string> = {
		off: 'Voice off', connecting: 'Voice connecting', listening: 'Listening on device',
		speaking: 'Device speaking', working: 'Voice working', muted: 'Device mic muted', error: 'Voice error'
	};
	let voiceLabel = $derived(snapshot ? snapshot.voiceConnecting ? voiceLabels.connecting :
		snapshot.voiceActive && snapshot.voiceMuted ? voiceLabels.muted :
		snapshot.state === 'recording' ? 'Recording on device' :
		snapshot.state === 'speaking' ? voiceLabels.speaking : voiceLabels[snapshot.voicePhase] : '');
	let latestMessage = $derived(snapshot?.messages[snapshot.messages.length - 1]);
	let caption = $derived.by(() => {
		if (!snapshot) return { label: '', text: '' };
		if (snapshot.lastError) return { label: 'Device error', text: snapshot.lastError };
		if (snapshot.voiceConnecting || snapshot.voiceMuted || snapshot.state === 'recording' ||
			snapshot.state === 'clock_sync' || snapshot.state === 'transcribing' || snapshot.state === 'thinking') {
			return { label: 'Device status', text: snapshot.status };
		}
		if (snapshot.voiceTranscript?.text && (snapshot.voiceActive || snapshot.state === 'speaking' || snapshot.voicePhase === 'working')) return {
			label: `${snapshot.voiceTranscript.role === 'user' ? 'Heard on device' : snapshot.buddyName}${snapshot.voiceTranscript.final ? '' : ' · live transcript'}`,
			text: snapshot.voiceTranscript.text
		};
		if (latestMessage?.role === 'assistant' && (snapshot.state === 'idle' || snapshot.state === 'speaking')) {
			return { label: 'Latest reply', text: latestMessage.text };
		}
		return { label: 'Device status', text: snapshot.status };
	});
</script>

<div class="state-preview">
	<div class="screen" class:screen-off={snapshot?.screenOff} class:dark={snapshot && scene.dark} aria-label="Native device state preview">
		{#if snapshot}
			<div class="drawing" class:dimmed={snapshot.screenOff} aria-hidden="true">
				<div class="background"><ArvelScene scene={snapshot.scene} {animate} /></div>
				<div class="bird"><ArvelBird size={132} {animate} /></div>
			</div>
			<header class="screen-head">
				<div class="identity"><strong>{snapshot.buddyName}</strong><span>{snapshot.model}</span></div>
				<time aria-label="Device clock">{snapshot.localTime || (snapshot.clockReady ? 'Clock ready' : 'Clock syncing')}</time>
				<a class="settings" href="/settings" aria-label="Open device settings" title="Open device settings">
					<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" aria-hidden="true"><path d="m9.5 3-.5 2-2 1-2-.5-2.5 4 1.5 1.5v2L2.5 15l2.5 4 2-.5 2 1 .5 2h5l.5-2 2-1 2 .5 2.5-4-1.5-2v-2L21.5 9 19 5l-2 .5-2-1-.5-2z" /><circle cx="12" cy="12" r="3" /></svg>
				</a>
			</header>
			<div class="bubble" class:error={snapshot.lastError || snapshot.state === 'error' || snapshot.voicePhase === 'error'} aria-hidden="true">
				<span class="caption-label">{caption.label}</span>
				<p>{caption.text}</p>
				<span class="voice-phase">{voiceLabel}</span>
			</div>
			{#if snapshot.screenOff}<span class="off-label">Physical screen off</span>{/if}
		{:else}
			<div class="unavailable" role="status"><p>{loading ? 'Checking owner access and device state…' : authenticated ? 'Arvel is unreachable. Retry the connection below.' : 'Log in to view Arvel’s device state.'}</p></div>
		{/if}
	</div>
	<div class="preview-status">
		<p role="status" aria-live="polite">{snapshot ? `${snapshot.screenOff ? 'Physical screen off · ' : ''}${snapshot.status} · ${voiceLabel}` : 'Device state unavailable'}</p>
		{#if snapshot}<button type="button" class="a-btn ghost" aria-pressed={animationsPaused} onclick={() => { animationsPaused = !animationsPaused; }}>{animationsPaused ? 'Resume preview animation' : 'Pause preview animation'}</button>{/if}
	</div>
	{#if snapshot && caption.text}<p class="caption"><strong>{caption.label}:</strong> {caption.text}</p>{/if}
	<p class="preview-note">Drawn from native device state · 640 × 172 layout · not exact screen pixels{snapshot ? ` · ${scene.name} scene` : ''}. Pausing animation does not stop device updates.</p>
</div>

<style>
	.state-preview { width: 100%; color: #e9eae6; }
	.screen { position: relative; isolation: isolate; container-type: inline-size; width: 100%; aspect-ratio: 640 / 172; overflow: hidden; border-radius: 4px; background: #17251f; color: #243d32; }
	.drawing, .background { position: absolute; inset: 0; }
	.drawing.dimmed { opacity: 0.35; }
	.bird { position: absolute; left: 3.75%; bottom: 2.3%; width: 20.625%; aspect-ratio: 1; }
	.bird :global(svg) { width: 100%; height: 100%; }
	.screen-head { position: absolute; top: 4.65%; left: 3.75%; right: 1.25%; display: flex; align-items: center; gap: 8px; z-index: 1; }
	.identity { display: flex; align-items: baseline; gap: 8px; min-width: 0; flex: 1; }
	.identity strong { font-size: clamp(11px, 2.5cqw, 20px); font-weight: 700; white-space: nowrap; max-width: 45%; overflow: hidden; text-overflow: ellipsis; }
	.identity span { font-size: clamp(10px, 1.875cqw, 15px); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
	time { font-size: clamp(10px, 1.875cqw, 15px); white-space: nowrap; font-variant-numeric: tabular-nums; }
	.settings { display: grid; place-items: center; width: 44px; height: 44px; flex-shrink: 0; border-radius: 8px; color: inherit; background: rgba(245, 247, 239, 0.88); }
	.settings:hover { background: #f4f6ef; }
	.settings:active { transform: translateY(1px); }
	.settings:focus-visible { outline: 3px solid #285c46; outline-offset: -3px; }
	.bubble { position: absolute; top: 34.9%; left: 27.5%; right: 3.75%; bottom: 9.3%; display: flex; flex-direction: column; gap: 4px; padding: 8px 12px; border-radius: 12px 12px 12px 4px; background: rgba(246, 248, 240, 0.96); color: #243d32; }
	.caption-label { font-size: clamp(9px, 1.56cqw, 12px); font-weight: 700; }
	.bubble p { margin: 0; font-size: clamp(11px, 2.2cqw, 18px); line-height: 1.25; overflow: hidden; display: -webkit-box; line-clamp: 2; -webkit-line-clamp: 2; -webkit-box-orient: vertical; overflow-wrap: anywhere; flex: 1; }
	.voice-phase { font-size: clamp(9px, 1.56cqw, 12px); font-weight: 650; }
	.error { background: #f6e5dd; color: #733929; }
	.dark .screen-head, .screen-off .screen-head { color: #f0f3eb; }
	.dark .identity, .dark time { background: rgba(24, 38, 33, 0.9); border-radius: 4px; padding: 4px; }
	.dark .settings, .screen-off .settings { color: #243d32; }
	.off-label { position: absolute; left: 2.5%; bottom: 9.3%; max-width: 23%; padding: 4px; border-radius: 4px; background: #17251f; color: #f0f3eb; font-size: clamp(10px, 1.875cqw, 15px); font-weight: 650; text-align: center; }
	.unavailable { position: absolute; inset: 0; display: grid; place-items: center; padding: 16px; color: #e9eae6; font-size: 14px; text-align: center; }
	.preview-status { display: flex; align-items: center; justify-content: space-between; flex-wrap: wrap; gap: 8px; margin-top: 12px; font-size: 13px; }
	.preview-status p { margin: 0; flex: 1; min-width: 20ch; }
	.preview-status .a-btn { min-height: 44px; }
	.caption { margin: 8px 0 0; font-size: 14px; line-height: 1.5; white-space: pre-wrap; overflow-wrap: anywhere; max-width: 75ch; }
	.preview-note { margin: 8px 0 0; color: #cdd2cd; font-size: 12px; line-height: 1.5; }
	@media (max-width: 560px) {
		.state-preview { color: var(--ink); }
		.preview-note { color: var(--sub); }
		.screen-head { top: 0; gap: 4px; }
		.identity { display: block; }
		.identity strong, .identity span { display: block; max-width: none; }
		.bubble { padding: 4px 8px; gap: 0; }
		.bubble p { line-clamp: 1; -webkit-line-clamp: 1; }
	}
</style>
