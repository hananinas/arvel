<script lang="ts" module>
	export type SettingsTab = 'voice' | 'display' | 'arvel' | 'device';
	export const settingsTabs: { id: SettingsTab; name: string }[] = [
		{ id: 'voice', name: 'Voice' },
		{ id: 'display', name: 'Display' },
		{ id: 'arvel', name: 'Arvel' },
		{ id: 'device', name: 'Device' }
	];
</script>

<script lang="ts">
	import { voiceStyles, screenSleeps, personas } from '$lib/buddySettings';
	import type { PreferenceKey, PreferenceValues } from '$lib/preferenceSaver.svelte';
	import { sceneById } from '$lib/arvelScenes';
	import type { ConversationMode } from '$lib/buddySettings';

	type Option = { id: string; name: string };
	type Row =
		| { kind: 'toggle'; label: string; key?: PreferenceKey; on: boolean; disabled?: boolean }
		| { kind: 'stepper'; label: string; key: PreferenceKey; value: number; text: string; step: number; min: number; max: number }
		| { kind: 'seg'; label: string; key: PreferenceKey; value: string; options: Option[] }
		| { kind: 'value'; label: string; text: string; go?: () => void };

	let {
		tab,
		values,
		model = '',
		ssid = '',
		firmware = '',
		disabled = false,
		conversationMode = 'text_pipeline',
		onchange,
		onscene
	}: {
		tab: SettingsTab;
		values: PreferenceValues;
		model?: string;
		ssid?: string;
		firmware?: string;
		disabled?: boolean;
		conversationMode?: ConversationMode;
		onchange: (key: PreferenceKey, value: string | number | boolean, delay?: number) => void;
		onscene?: () => void;
	} = $props();

	const pct = (v: number) => `${v}%`;
	const unavailable = 'Not available on this device yet';

	let rows = $derived.by((): Row[] => {
		switch (tab) {
			case 'voice':
				if (conversationMode !== 'text_pipeline') return [
					{ kind: 'toggle', label: 'Live captions', key: 'captions', on: values.captions }
				];
				return [
					{ kind: 'seg', label: 'Voice', key: 'voiceStyle', value: values.voiceStyle, options: voiceStyles },
					{ kind: 'stepper', label: 'Speaking speed', key: 'speechRate', value: values.speechRate, text: `${(values.speechRate / 10).toFixed(1)}×`, step: 1, min: 5, max: 20 },
					{ kind: 'toggle', label: 'Live captions', key: 'captions', on: values.captions }
				];
			case 'display':
				return [
					{ kind: 'stepper', label: 'Brightness', key: 'brightness', value: values.brightness, text: pct(values.brightness), step: 10, min: 10, max: 100 },
					{ kind: 'value', label: 'Scene', text: sceneById(values.scene).name, go: onscene },
					{ kind: 'toggle', label: 'Dim at night', key: 'dimAtNight', on: values.dimAtNight },
					{ kind: 'seg', label: 'Screen sleep', key: 'screenSleep', value: values.screenSleep, options: screenSleeps }
				];
			case 'arvel':
				return [
					{ kind: 'seg', label: 'Personality', key: 'persona', value: values.persona, options: personas },
					{ kind: 'value', label: 'Model', text: model || 'Not set' },
					...(conversationMode === 'text_pipeline' ? [{ kind: 'toggle' as const, label: 'Read replies aloud', key: 'readAloud' as const, on: values.readAloud }] : []),
					{ kind: 'toggle', label: 'Approve actions for me', on: false, disabled: true }
				];
			default:
				return [
					{ kind: 'stepper', label: 'Volume', key: 'volume', value: values.volume, text: pct(values.volume), step: 10, min: 0, max: 100 },
					{ kind: 'value', label: 'Wi-Fi', text: ssid || 'Not connected' },
					{ kind: 'value', label: 'Paired desktop', text: 'Not paired' },
					{ kind: 'value', label: 'Firmware', text: firmware || 'Unknown' }
				];
		}
	});
</script>

<div class="rows">
	{#each rows as row (row.label)}
		<div class="row" class:unavailable={row.kind === 'toggle' && row.disabled} title={row.kind === 'toggle' && row.disabled ? unavailable : undefined}>
			<span class="row-label">{row.label}</span>
			{#if row.kind === 'toggle'}
				<button
					type="button"
					class="switch"
					role="switch"
					aria-checked={row.on}
					aria-label={row.label}
					aria-disabled={row.disabled || disabled ? 'true' : undefined}
					style:background={row.on ? 'var(--a-accent)' : 'var(--a-toggleOff)'}
					onclick={() => { if (!row.disabled && !disabled && row.key) onchange(row.key, !row.on); }}
				><span class="knob" style:margin-left={row.on ? '19px' : '3px'}></span></button>
			{:else if row.kind === 'stepper'}
				<div class="stepper">
					<button type="button" aria-label={`Decrease ${row.label}`} disabled={disabled || row.value <= row.min}
						onclick={() => onchange(row.key, Math.max(row.min, row.value - row.step), 600)}>−</button>
					<span class="step-value" aria-live="polite">{row.text}</span>
					<button type="button" aria-label={`Increase ${row.label}`} disabled={disabled || row.value >= row.max}
						onclick={() => onchange(row.key, Math.min(row.max, row.value + row.step), 600)}>+</button>
				</div>
			{:else if row.kind === 'seg'}
				<div class="seg" role="radiogroup" aria-label={row.label}>
					{#each row.options as option (option.id)}
						<button type="button" role="radio" aria-checked={row.value === option.id} class:on={row.value === option.id} {disabled}
							onclick={() => onchange(row.key, option.id)}>{option.name}</button>
					{/each}
				</div>
			{:else}
				<button type="button" class="value" class:fixed-value={!row.go} disabled={!row.go} onclick={() => row.go?.()}>
					<span>{row.text}</span>
					<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m9 18 6-6-6-6"></path></svg>
				</button>
			{/if}
		</div>
	{/each}
	{#if tab === 'voice'}<p class="wake-note">Voice starts only after an explicit microphone action on Arvel or through owner controls. There are no background wake-word uploads; local speech uses your configured STT service.</p>{/if}
</div>

<style>
	.rows { display: flex; flex-direction: column; justify-content: center; min-width: 0; }
	.row { height: 32px; display: flex; align-items: center; gap: 8px; border-bottom: 1px solid var(--a-rule); }
	.row:last-child { border-bottom-color: transparent; }
	.row-label { flex-grow: 1; min-width: 0; font-size: 13px; font-weight: 550; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
	.wake-note { font-size: 11px; line-height: 1.5; color: var(--a-sub); margin: 8px 0 0; max-width: 65ch; }
	.unavailable { opacity: 0.45; }
	button { font: inherit; margin: 0; border: none; cursor: pointer; }
	button:disabled, button[aria-disabled='true'] { cursor: not-allowed; }
	.switch { width: 40px; height: 24px; border-radius: 12px; padding: 0; display: flex; align-items: center; flex-shrink: 0; }
	.knob { width: 18px; height: 18px; border-radius: 9px; background: #ffffff; box-shadow: 0 1px 3px rgba(0, 0, 0, 0.25); }
	.stepper { display: flex; align-items: center; gap: 4px; flex-shrink: 0; }
	.stepper button { width: 28px; height: 26px; border-radius: 13px; padding: 0; background: var(--a-chipBg); color: var(--a-ink); font-size: 15px; font-weight: 600; }
	.stepper button:disabled { opacity: 0.35; }
	.step-value { min-width: 44px; text-align: center; font-size: 13px; font-weight: 650; font-variant-numeric: tabular-nums; }
	.seg { display: flex; gap: 2px; padding: 2px; border-radius: 9px; background: var(--a-chipBg); flex-shrink: 0; }
	.seg button { height: 24px; padding: 0 9px; border-radius: 7px; background: transparent; color: var(--a-ink); font-size: 12px; font-weight: 500; }
	.seg button.on { background: var(--a-segOn); font-weight: 650; box-shadow: 0 1px 3px rgba(0, 0, 0, 0.12); }
	.value { display: flex; align-items: center; gap: 4px; height: 28px; background: none; padding: 0 2px; color: var(--a-sub); font-size: 13px; flex-shrink: 0; max-width: 60%; }
	.value span { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
	.value.fixed-value { cursor: default; }
	.value:disabled { opacity: 1; }
	.value:not(.fixed-value):hover span { text-decoration: underline; text-underline-offset: 3px; }
</style>
