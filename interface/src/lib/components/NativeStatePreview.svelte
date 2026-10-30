<script lang="ts">
	import type { AgentSnapshot, AgentTool, VoicePhase } from '$lib/agent';
	import { sceneById } from '$lib/arvelScenes';
	import { MIC, READY, THINKING, palette, paletteStyle } from '$lib/theme';
	import { toolIconMarkup, toolKind, type ToolKind } from '$lib/arvelToolIcons';
	import ArvelScene from './ArvelScene.svelte';
	import ArvelBird from './ArvelBird.svelte';

	let { snapshot, authenticated = false, loading = false }: {
		snapshot: AgentSnapshot | null;
		authenticated?: boolean;
		loading?: boolean;
	} = $props();

	// The case is drawn at the design's real pixel size (750 × 265 around a 640 × 172 screen) and scaled as one unit.
	const CASE_W = 750, CASE_H = 265;
	let width = $state(CASE_W);
	let scale = $derived(Math.min(1.5, width / CASE_W));
	let animationsPaused = $state(false);
	let stage: HTMLDivElement | undefined = $state();

	let scene = $derived(sceneById(snapshot?.scene));
	let dark = $derived(scene.dark);
	let pal = $derived(palette(dark));
	let toolChipBg = $derived(dark ? 'rgba(255,255,255,0.08)' : '#F2F2EE');
	let animate = $derived(!animationsPaused && !snapshot?.screenOff);

	type Mode = 'idle' | 'listening' | 'live' | 'thinking' | 'speaking';
	// Mirrors activity_mode() and refresh_status() in components/ui_bsp/buddy_ui.c.
	let mode = $derived.by((): Mode => {
		const s = snapshot;
		if (!s) return 'idle';
		if (s.voiceActive || s.voiceConnecting) return 'live';
		if (s.state === 'recording' || s.state === 'listening') return 'listening';
		if (s.state === 'thinking' || s.state === 'transcribing' || s.state === 'clock_sync') return 'thinking';
		if (s.state === 'speaking') return 'speaking';
		return 'idle';
	});
	let statusLabel = $derived.by(() => {
		const s = snapshot;
		if (!s) return 'READY';
		if (mode === 'live') {
			if (!s.wifiConnected) return 'OFFLINE';
			if (s.voiceMuted) return s.voiceConnecting ? 'MUTED · CONNECTING' : 'MIC MUTED';
			if (s.voiceConnecting) return 'CONNECTING';
			if (s.state === 'thinking' || s.voicePhase === 'working') return 'WORKING';
			if (s.state === 'speaking' || s.voicePhase === 'speaking') return 'SPEAKING';
			return 'LISTENING';
		}
		if (mode === 'idle' && (s.state === 'error' || s.lastError)) return 'ERROR';
		return { idle: 'READY', listening: 'LISTENING', thinking: 'THINKING', speaking: 'SPEAKING' }[mode];
	});
	let dotColor = $derived(statusLabel === 'ERROR' || mode === 'listening' ? MIC :
		mode === 'thinking' ? THINKING : mode === 'speaking' ? pal.accent : READY);
	let bobDur = $derived(mode === 'speaking' ? '0.55s' : mode === 'listening' ? '1.6s' : '3.2s');

	let clock = $derived(snapshot?.clockReady ? snapshot.localTime.match(/\d{1,2}:\d{2}/)?.[0] ?? '--:--' : '--:--');
	let hour = $derived(Number(clock.split(':')[0]));
	// Mirrors the device status row, which omits Codex's fixed transport model.
	let shownModel = $derived(snapshot?.conversationMode === 'codex_live' ? '' : snapshot?.model ?? '');
	let userName = $derived(snapshot?.preferences.userName?.trim() ?? '');
	let lastUser = $derived(snapshot?.messages.findLast((m) => m.role === 'user')?.text ?? '');
	let lastReply = $derived(snapshot?.messages.findLast((m) => m.role === 'assistant')?.text ?? '');

	let greeting = $derived.by(() => {
		if (!snapshot) return '';
		if (snapshot.state === 'error' || snapshot.lastError) return 'Request needs attention';
		const ready = snapshot.clockReady && clock !== '--:--';
		const part = !ready ? 'Hello' : hour < 12 ? 'Morning' : hour < 18 ? 'Afternoon' : 'Evening';
		if (userName) return `${part}, ${userName}.`;
		return ready ? `Good ${part.toLowerCase()}.` : 'Hello there.';
	});
	let idleSub = $derived.by(() => {
		const s = snapshot;
		if (!s) return '';
		if (s.lastError) return s.lastError;
		if (!s.wifiConnected) return 'Offline — open Settings > Device > Wi-Fi for local setup.';
		if (!s.chatConfigured && !s.voiceLiveAvailable) return 'Open browser Settings to choose a conversation model and configure its credentials.';
		if (s.voiceLiveAvailable) return 'Tap mic to start or mute · tap End to stop.';
		if (!s.voiceConfigured) return 'Open browser Settings to configure the selected conversation transport.';
		return 'Tap the mic to talk — I’m right here if you need a hand.';
	});
	const suggestions: { kind: ToolKind; label: string }[] = [
		{ kind: 'timer', label: 'Focus 25m' }, { kind: 'notes', label: 'To-dos' },
		{ kind: 'calc', label: 'Convert' }, { kind: 'weather', label: 'Weather' }
	];

	let thinkTitle = $derived(snapshot?.state === 'clock_sync' ? 'Setting the clock' :
		snapshot?.state === 'transcribing' ? 'Catching your words' : 'Thinking it over');
	let thinkTools = $derived(snapshot?.state === 'thinking' ? snapshot.tools.slice(-2) : []);

	let liveCaption = $derived(snapshot?.preferences.captions !== false && snapshot?.voiceTranscript?.text ? snapshot.voiceTranscript : null);
	let liveEmpty = $derived(!snapshot?.wifiConnected ? 'Voice chat is offline' : snapshot.voiceConnecting ? 'Opening voice chat…' :
		snapshot.voiceMuted ? 'Microphone muted' : snapshot.state === 'thinking' ? 'Working on your request' :
		snapshot.state === 'speaking' ? 'Playing voice reply' : 'Voice chat is open');
	let liveHint = $derived(!snapshot?.wifiConnected ? 'Offline · tap End to stop' :
		snapshot.voiceMuted ? 'Mic muted · tap mic to unmute · End to stop' :
		snapshot.voiceConnecting ? 'Connecting · tap End to stop' : 'Mic on · tap mic to mute · End to stop');

	const voiceLabels: Record<VoicePhase, string> = {
		off: 'Voice off', connecting: 'Voice connecting', listening: 'Listening on device',
		speaking: 'Device speaking', working: 'Voice working', muted: 'Device mic muted', error: 'Voice error'
	};
	let voiceLabel = $derived(snapshot ? snapshot.voiceConnecting ? voiceLabels.connecting :
		snapshot.voiceActive && snapshot.voiceMuted ? voiceLabels.muted :
		snapshot.state === 'recording' ? 'Recording on device' :
		snapshot.state === 'speaking' ? voiceLabels.speaking : voiceLabels[snapshot.voicePhase] : '');
	// A text equivalent of the drawn screen for assistive technology.
	let screenText = $derived.by(() => {
		if (!snapshot) return '';
		const body = mode === 'idle' ? `${greeting} ${idleSub}` : mode === 'listening' ? 'Listening.' :
			mode === 'live' ? liveCaption ? `${liveCaption.role === 'user' ? 'You' : snapshot.buddyName}: ${liveCaption.text}` : liveEmpty :
			mode === 'thinking' ? `${thinkTitle}. ${thinkTools.map((t) => `${t.verb} ${t.target}`).join(', ')}` : lastReply;
		return `${statusLabel}${shownModel ? ` · ${shownModel}` : ''}. ${body}`;
	});

	const toolStates: Record<AgentTool['state'], string> = {
		running: 'Running', succeeded: 'Succeeded', failed: 'Failed', unknown: 'Unknown', cancelled: 'Cancelled', partial: 'Partial'
	};
	const bars = [
		[0, 9, 6, '6;18;6', '9;3;9', 0.9], [9, 6, 12, '12;22;8;12', '6;1;8;6', 1.1], [18, 4, 16, '16;6;20;16', '4;9;2;4', 0.8],
		[27, 8, 8, '8;20;8', '8;2;8', 1], [36, 2, 20, '20;10;20', '2;7;2', 0.7], [45, 7, 10, '10;22;10', '7;1;7', 1.2],
		[54, 5, 14, '14;6;14', '5;9;5', 0.85], [63, 9, 6, '6;16;6', '9;4;9', 0.95], [72, 6, 12, '12;20;12', '6;2;6', 1.05],
		[81, 9, 6, '6;12;6', '9;6;9', 0.75]
	] as const;

	// The bird and scene pause themselves; this pauses the screen's own SMIL (ripples, meter, dots, spinners).
	$effect(() => {
		void mode; void thinkTools; void snapshot?.voiceMuted;
		if (!stage) return;
		const reduce = window.matchMedia('(prefers-reduced-motion: reduce)');
		const apply = () => stage?.querySelectorAll<SVGSVGElement>('svg.anim').forEach((svg) =>
			animate && !reduce.matches ? svg.unpauseAnimations() : svg.pauseAnimations());
		apply();
		reduce.addEventListener('change', apply);
		return () => reduce.removeEventListener('change', apply);
	});
</script>

{#snippet toolIcon(kind: string | number, size: number)}
	<svg width={size} height={size} viewBox="0 0 32 32" aria-hidden="true">{@html toolIconMarkup[toolKind(kind)]}</svg>
{/snippet}

<div class="state-preview">
	<div class="fit" bind:clientWidth={width} style:height="{CASE_H * scale}px">
		<div class="case" style:transform="scale({scale})">
			<div class="bezel">
				<div class="screen" bind:this={stage} class:off={snapshot?.screenOff} style="{paletteStyle(pal)};--chip-tool:{toolChipBg}"
					role="img" aria-label={snapshot ? `Drawing of Arvel’s screen: ${screenText}` : 'Arvel’s screen'}>
					{#if snapshot}
						<div class="layer scene"><ArvelScene scene={snapshot.scene} {animate} /></div>

						{#if mode === 'listening'}
							<svg class="layer ripples anim" width="180" height="172" viewBox="0 0 180 172" aria-hidden="true">
								{#each [0, 0.8] as begin}
									<circle cx="88" cy="96" r="40" fill="none" stroke={MIC} stroke-width="2">
										<animate attributeName="r" values="34;72" dur="1.6s" begin="{begin}s" repeatCount="indefinite" />
										<animate attributeName="opacity" values="0.55;0" dur="1.6s" begin="{begin}s" repeatCount="indefinite" />
									</circle>
								{/each}
							</svg>
						{/if}

						<div class="bird"><ArvelBird size={170} {bobDur} {animate} /></div>

						<div class="pill">
							<span>{clock}</span>
							<svg width="13" height="13" viewBox="0 0 24 24" class="stroke" stroke-width="2.4" aria-hidden="true" opacity={snapshot.wifiConnected ? 1 : 0.35}><path d="M12 20h.01" /><path d="M2 8.82a15 15 0 0 1 20 0" /><path d="M5 12.859a10 10 0 0 1 14 0" /><path d="M8.5 16.429a5 5 0 0 1 7 0" /></svg>
							<svg width="14" height="14" viewBox="0 0 24 24" class="stroke" stroke-width="2.2" aria-hidden="true"><path d="M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z" /><circle cx="12" cy="12" r="3" /></svg>
						</div>

						<div class="bubble">
							<div class="tail"></div>
							<div class="status-row">
								<span class="dot" style:background={dotColor}></span>
								<span class="stat-label">{statusLabel}</span>
								<span class="grow"></span>
								<span class="model">{shownModel}</span>
							</div>

							{#if mode === 'idle'}
								<div class="content">
									<div class="greeting">{greeting}</div>
									<div class="sub clamp2">{idleSub}</div>
									<div class="grow"></div>
									<div class="chips">
										{#each suggestions as s}
											<span class="chip">{@render toolIcon(s.kind, 22)}<span>{s.label}</span></span>
										{/each}
									</div>
								</div>
							{:else if mode === 'listening'}
								<div class="content gap6">
									<div class="heard">Go ahead — I’m listening <span class="caret">|</span></div>
									<div class="grow"></div>
									<div class="meter-row">
										<svg class="anim" width="132" height="24" viewBox="0 0 132 24" fill={MIC} aria-hidden="true">
											{#each bars as [x, y, h, hs, ys, dur]}
												<rect {x} {y} width="4" height={h} rx="2">
													<animate attributeName="height" values={hs} dur="{dur}s" repeatCount="indefinite" />
													<animate attributeName="y" values={ys} dur="{dur}s" repeatCount="indefinite" />
												</rect>
											{/each}
										</svg>
										<span class="hint">Tap ■ when you’re done</span>
									</div>
								</div>
							{:else if mode === 'live'}
								<div class="content gap6">
									{#if liveCaption}
										<span class="stat-label">{liveCaption.role === 'user' ? 'YOU' : snapshot.buddyName.toUpperCase()}</span>
										<div class="words clamp3 live-words">{liveCaption.text}</div>
									{:else}
										<div class="heard">{liveEmpty}</div>
									{/if}
									<div class="grow"></div>
									<span class="hint">{liveHint}</span>
								</div>
							{:else if mode === 'thinking'}
								<div class="content gap6">
									<div class="think-head">
										<span class="think-title">{thinkTitle}</span>
										<svg class="anim" width="30" height="10" viewBox="0 0 30 10" fill="var(--a-accent)" aria-hidden="true">
											{#each [0, 0.2, 0.4] as begin, i}
												<circle cx={5 + 10 * i} cy="5" r="3.5"><animate attributeName="opacity" values="0.25;1;0.25" dur="1.2s" begin="{begin}s" repeatCount="indefinite" /></circle>
											{/each}
										</svg>
									</div>
									{#if thinkTools.length}
										<div class="tool-rows">
											{#each thinkTools as tool (tool.id || tool.verb)}
												<div class="tool-row">
													{@render toolIcon(tool.kind, 22)}
													<span class="verb">{tool.verb} · {toolStates[tool.state]}</span>
													<span class="target mono">{[tool.error, tool.target].filter(Boolean).join(' · ')}</span>
													<span class="grow"></span>
													{#if tool.state === 'running'}
														<svg class="anim" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="var(--a-accent)" stroke-width="3" stroke-linecap="round" aria-hidden="true"><path d="M12 3a9 9 0 1 0 9 9"><animateTransform attributeName="transform" type="rotate" from="0 12 12" to="360 12 12" dur="0.9s" repeatCount="indefinite" /></path></svg>
													{:else if tool.state === 'succeeded'}
														<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="var(--a-accent)" stroke-width="3" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M20 6 9 17l-5-5" /></svg>
													{/if}
												</div>
											{/each}
										</div>
									{:else if lastUser && snapshot.state === 'thinking'}
										<div class="sub clamp2">“{lastUser}”</div>
									{/if}
									<div class="grow"></div>
									<div class="foot">
										<svg width="13" height="13" viewBox="0 0 24 24" class="stroke sub-stroke" stroke-width="2.2" aria-hidden="true"><path d="M20 13c0 5-3.5 7.5-7.66 8.95a1 1 0 0 1-.67-.01C7.5 20.5 4 18 4 13V6a1 1 0 0 1 1-1c2 0 4.5-1.2 6.24-2.72a1.17 1.17 0 0 1 1.52 0C14.51 3.81 17 5 19 5a1 1 0 0 1 1 1z" /><path d="m9 12 2 2 4-4" /></svg>
										<span class="ellipsis">Using {shownModel || 'your model'} through your API</span>
									</div>
								</div>
							{:else}
								<div class="content gap6">
									<div class="words clamp3">{lastReply}</div>
									<div class="grow"></div>
									<div class="foot">
										<svg class="anim" width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="var(--a-accent)" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M11 4.702a.705.705 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298z" /><path d="M16 9a5 5 0 0 1 0 6"><animate attributeName="opacity" values="0.3;1;0.3" dur="0.9s" repeatCount="indefinite" /></path><path d="M19.364 18.364a9 9 0 0 0 0-12.728"><animate attributeName="opacity" values="0.3;1;0.3" dur="0.9s" begin="0.3s" repeatCount="indefinite" /></path></svg>
										<span>Speaking · tap the mic to interrupt</span>
									</div>
								</div>
							{/if}
						</div>

						<div class="rail">
							{#if snapshot.voiceActive || snapshot.voiceConnecting}
								<span class="round end">End</span>
							{:else}
								<span class="round"><svg width="19" height="19" viewBox="0 0 24 24" class="stroke rail-stroke" stroke-width="2" aria-hidden="true"><rect width="18" height="18" x="3" y="3" rx="2" /><circle cx="9" cy="9" r="2" /><path d="m21 15-3.086-3.086a2 2 0 0 0-2.828 0L6 21" /></svg></span>
							{/if}
							<span class="round"><svg width="19" height="19" viewBox="0 0 24 24" class="stroke rail-stroke" stroke-width="2" aria-hidden="true"><path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z" /></svg></span>
							<span class="round mic">
								{#if mode === 'listening'}
									<svg width="16" height="16" viewBox="0 0 24 24" aria-hidden="true"><rect x="5" y="5" width="14" height="14" rx="3" fill="#FFFFFF" /></svg>
								{:else}
									<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="#FFFFFF" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 2a3 3 0 0 0-3 3v7a3 3 0 0 0 6 0V5a3 3 0 0 0-3-3Z" /><path d="M19 10v2a7 7 0 0 1-14 0v-2" /><line x1="12" x2="12" y1="19" y2="22" />{#if snapshot.voiceMuted}<line x1="3" y1="3" x2="21" y2="21" stroke="#F8F6F0" />{/if}</svg>
								{/if}
							</span>
						</div>

						{#if snapshot.screenOff}<div class="layer sleep"><span>Screen off</span></div>{/if}
					{:else}
						<div class="layer unavailable" role="status">
							<p>{loading ? 'Checking owner access and device state…' : authenticated ? 'Arvel is unreachable. Retry the connection below.' : 'Log in to view Arvel’s screen.'}</p>
						</div>
					{/if}
				</div>
			</div>
		</div>
	</div>

	<div class="preview-bar" style:max-width="{CASE_W * scale}px">
		<div class="chips-out" role="status" aria-live="polite">
			{#if snapshot}
				<span class="tag"><span class="tag-dot" style:background={dotColor}></span>{snapshot.status || statusLabel}</span>
				<span class="tag">{voiceLabel}</span>
				<span class="tag">{scene.name} scene</span>
				{#if snapshot.screenOff}<span class="tag">Physical screen off</span>{/if}
			{:else}
				<span class="tag">Device state unavailable</span>
			{/if}
		</div>
		{#if snapshot}
			<button type="button" class="a-btn ghost" aria-pressed={animationsPaused} onclick={() => { animationsPaused = !animationsPaused; }}>{animationsPaused ? 'Resume animation' : 'Pause animation'}</button>
		{/if}
	</div>
</div>

<style>
	.state-preview { width: 100%; display: flex; flex-direction: column; align-items: center; gap: 14px; }
	.fit { width: 100%; max-width: 1125px; position: relative; }
	.case {
		position: absolute; left: 50%; top: 0; width: 750px; height: 265px; margin-left: -375px; transform-origin: top center;
		display: flex; align-items: center; justify-content: center; border-radius: 20px; background: #4b8fd9;
		box-shadow: 0 22px 50px rgba(40, 52, 48, 0.22), inset 0 -6px 0 rgba(0, 0, 0, 0.12), inset 0 3px 0 rgba(255, 255, 255, 0.25);
	}
	.bezel { width: 706px; height: 221px; display: flex; align-items: center; justify-content: center; border-radius: 10px; background: #0b0c0d; box-shadow: inset 0 0 0 1px rgba(255, 255, 255, 0.06); }

	/* Everything inside .screen uses the device's own pixel coordinates from buddy_ui.c. */
	.screen { position: relative; isolation: isolate; width: 640px; height: 172px; overflow: hidden; border-radius: 2px; background: #dcebe4; color: var(--a-ink); font-family: 'Inter', system-ui, sans-serif; font-size: 13px; line-height: normal; user-select: none; }
	.layer { position: absolute; inset: 0; }
	.ripples { right: auto; bottom: auto; }
	.bird { position: absolute; left: -4px; top: 18px; width: 170px; height: 170px; }
	.bird :global(svg) { display: block; }
	.stroke { fill: none; stroke: var(--a-ink); stroke-linecap: round; stroke-linejoin: round; }
	.sub-stroke { stroke: var(--a-sub); }
	.rail-stroke { stroke: var(--a-railInk); }

	.pill { position: absolute; left: 8px; top: 8px; height: 32px; display: flex; align-items: center; gap: 7px; padding: 0 11px; border-radius: 16px; background: var(--a-railBg); color: var(--a-ink); font-size: 12px; font-weight: 600; font-variant-numeric: tabular-nums; box-shadow: 0 2px 8px rgba(30, 40, 36, 0.1); }

	.bubble { position: absolute; left: 176px; top: 12px; width: 396px; height: 148px; box-sizing: border-box; display: flex; flex-direction: column; gap: 5px; padding: 11px 16px 12px; border-radius: 16px; background: var(--a-bubbleBg); color: var(--a-ink); box-shadow: 0 6px 18px rgba(30, 40, 36, 0.14); }
	.tail { position: absolute; left: -6px; top: 58px; width: 14px; height: 14px; transform: rotate(45deg); border-radius: 3px; background: var(--a-bubbleBg); }
	.status-row { position: relative; display: flex; align-items: center; gap: 6px; height: 16px; flex-shrink: 0; }
	.dot { width: 7px; height: 7px; border-radius: 4px; flex-shrink: 0; }
	.stat-label { font-size: 11px; font-weight: 700; letter-spacing: 0.6px; color: var(--a-sub); white-space: nowrap; }
	.model { max-width: 200px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font-size: 11px; font-weight: 500; color: var(--a-sub); }
	.grow { flex-grow: 1; }
	.content { position: relative; display: flex; flex-direction: column; gap: 5px; flex-grow: 1; min-height: 0; }
	.gap6 { gap: 6px; }
	.greeting { font-size: 21px; font-weight: 650; letter-spacing: -0.4px; line-height: 1.2; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
	.sub { font-size: 14px; line-height: 1.35; color: var(--a-sub); overflow-wrap: anywhere; }
	.clamp2, .clamp3 { display: -webkit-box; -webkit-box-orient: vertical; overflow: hidden; }
	.clamp2 { -webkit-line-clamp: 2; line-clamp: 2; }
	.clamp3 { -webkit-line-clamp: 3; line-clamp: 3; }
	.chips { display: flex; gap: 5px; }
	.chip { display: flex; align-items: center; gap: 5px; height: 28px; padding: 0 10px 0 3px; border-radius: 14px; background: var(--chip-tool); font-size: 12px; font-weight: 600; white-space: nowrap; }
	.heard { min-height: 48px; font-size: 18px; font-weight: 550; line-height: 1.3; letter-spacing: -0.2px; }
	.caret { color: #a65b48; font-weight: 400; }
	.meter-row, .foot, .think-head { display: flex; align-items: center; }
	.meter-row { gap: 10px; }
	.think-head { gap: 10px; }
	.foot { gap: 6px; font-size: 12px; color: var(--a-sub); min-width: 0; }
	.hint { font-size: 12px; color: var(--a-sub); }
	.think-title { font-size: 17px; font-weight: 650; letter-spacing: -0.3px; }
	.tool-rows { display: flex; flex-direction: column; gap: 4px; }
	.tool-row { display: flex; align-items: center; gap: 7px; height: 24px; min-width: 0; }
	.tool-row :global(svg) { flex-shrink: 0; }
	.verb { font-size: 12.5px; font-weight: 650; white-space: nowrap; }
	.target { min-width: 0; max-width: 200px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font-size: 12px; color: var(--a-sub); }
	.mono { font-family: ui-monospace, 'SF Mono', Menlo, monospace; }
	.ellipsis { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
	.words { font-size: 17px; font-weight: 550; line-height: 1.36; letter-spacing: -0.15px; overflow-wrap: anywhere; }
	.live-words { line-height: 23px; }

	.rail { position: absolute; right: 10px; top: 12px; display: flex; flex-direction: column; gap: 8px; }
	.round { width: 44px; height: 44px; display: flex; align-items: center; justify-content: center; border-radius: 22px; background: var(--a-railBg); box-shadow: 0 2px 8px rgba(30, 40, 36, 0.12); }
	.round.mic { background: #a65b48; box-shadow: 0 3px 10px rgba(120, 50, 35, 0.3); }
	.round.end { background: #a65b48; color: #f8f6f0; font-size: 12px; font-weight: 600; }

	.sleep { display: flex; align-items: flex-end; justify-content: flex-start; padding: 14px; background: rgba(8, 10, 11, 0.82); z-index: 2; }
	.sleep span { padding: 4px 10px; border-radius: 10px; background: rgba(255, 255, 255, 0.1); color: #e6e9e4; font-size: 12px; font-weight: 600; }
	.unavailable { display: grid; place-items: center; padding: 16px; background: #17251f; color: #e9eae6; font-size: 14px; text-align: center; }

	.preview-bar { width: 100%; display: flex; align-items: center; justify-content: space-between; flex-wrap: wrap; gap: 8px 12px; }
	.chips-out { display: flex; flex-wrap: wrap; gap: 6px; min-width: 0; }
	.tag { display: inline-flex; align-items: center; gap: 6px; min-height: 28px; padding: 0 11px; border-radius: 14px; background: var(--paper); box-shadow: 0 1px 2px rgba(30, 40, 36, 0.08); color: var(--ink); font-size: 12px; font-weight: 550; overflow-wrap: anywhere; }
	.tag-dot { width: 7px; height: 7px; border-radius: 4px; flex-shrink: 0; }
	.preview-bar .a-btn { min-height: 36px; }
</style>
