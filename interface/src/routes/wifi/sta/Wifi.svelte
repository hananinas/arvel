<script lang="ts">
	import { onMount } from 'svelte';
	import { deviceRequest } from '$lib/agent';

	interface WifiStatus { status: number; ssid?: string; local_ip?: string; rssi?: number; }
	interface WifiSettings { wifi_networks: { ssid: string; password: string }[]; }
	interface Network { ssid: string; rssi: number; encryption: number; }
	let status = $state<WifiStatus | null>(null);
	let ssid = $state('');
	let password = $state('');
	let networks = $state<Network[]>([]);
	let loading = $state(true);
	let saving = $state(false);
	let scanning = $state(false);
	let error = $state('');
	let notice = $state('');
	let scanComplete = $state(false);
	let mounted = false;
	let timer: ReturnType<typeof setTimeout>;
	const requests = new Set<AbortController>();
	const textEncoder = new TextEncoder();
	let ssidBytes = $derived(textEncoder.encode(ssid).length);
	let passwordBytes = $derived(textEncoder.encode(password).length);

	async function refreshStatus() {
		const controller = new AbortController();
		requests.add(controller);
		try {
			const data = await deviceRequest<WifiStatus>('/rest/wifiStatus', { signal: controller.signal }, 5000);
			if (mounted) status = data;
		} catch {
			if (mounted) status = null;
		} finally {
			requests.delete(controller);
			if (mounted) timer = setTimeout(refreshStatus, 5000);
		}
	}

	async function loadSettings() {
		const controller = new AbortController();
		requests.add(controller);
		try {
			const data = await deviceRequest<WifiSettings>('/rest/wifiSettings', { signal: controller.signal });
			if (mounted) ssid = data.wifi_networks[0]?.ssid || '';
		} catch (failure) {
			if (mounted) error = failure instanceof Error ? failure.message : 'Wi-Fi settings could not be loaded. Reconnect and reload this page.';
		} finally {
			requests.delete(controller);
			if (mounted) loading = false;
		}
	}

	async function save(event: SubmitEvent) {
		event.preventDefault();
		if (saving || loading || ssidBytes < 1 || ssidBytes > 32 || passwordBytes > 63 || (passwordBytes > 0 && passwordBytes < 8)) return;
		saving = true;
		error = '';
		notice = '';
		const controller = new AbortController();
		requests.add(controller);
		try {
			await deviceRequest<WifiSettings>('/rest/wifiSettings', {
				method: 'POST', signal: controller.signal,
				body: JSON.stringify({ wifi_networks: [{ ssid, password, static_ip_config: false }] })
			});
			if (!mounted) return;
			password = '';
			notice = 'Wi-Fi configuration saved. The device is reconnecting. If its address changes, open the new device address to return to chat.';
		} catch (failure) {
			if (mounted) error = failure instanceof Error ? failure.message : 'Wi-Fi setup failed. Your entered settings have been kept.';
		} finally {
			requests.delete(controller);
			if (mounted) saving = false;
		}
	}

	async function scan() {
		if (scanning) return;
		scanning = true;
		error = '';
		scanComplete = false;
		const controller = new AbortController();
		requests.add(controller);
		try {
			const data = await deviceRequest<{ networks: Network[] }>('/rest/wifiScan', { signal: controller.signal }, 20000);
			if (mounted) {
				networks = data.networks.filter((network) => network.ssid.length > 0);
				scanComplete = true;
			}
		} catch (failure) {
			if (mounted) error = failure instanceof Error ? failure.message : 'Network scan failed. You can enter a network name manually.';
		} finally {
			requests.delete(controller);
			if (mounted) scanning = false;
		}
	}

	onMount(() => {
		mounted = true;
		void loadSettings();
		void refreshStatus();
		return () => {
			mounted = false;
			clearTimeout(timer);
			for (const controller of requests) controller.abort();
			requests.clear();
		};
	});
</script>

<section class="wifi-setup" aria-labelledby="wifi-heading">
	<a class="back-link" href="/">← Back to conversation</a>
	<p class="eyebrow">Keep your Buddy connected</p>
	<h1 id="wifi-heading">Wi-Fi setup</h1>
	<p class="setup-copy">Connect the device to a 2.4 GHz network so Buddy can reach its cloud model. This device saves one network and uses automatic IP configuration.</p>
	<div class="connection-summary" role="status" aria-live="polite"><strong>{status ? (status.status === 3 ? 'Device Wi-Fi connected' : 'Device Wi-Fi offline') : (loading ? 'Reading device status…' : 'Device status unavailable')}</strong>{#if status?.status === 3}<span>{status.ssid} · {status.local_ip || 'IP address pending'} · {status.rssi} dBm</span>{/if}</div>
	{#if error}<div class="notice error" role="alert">{error}</div>{/if}
	{#if notice}<div class="notice" role="status">{notice}</div>{/if}
	<form onsubmit={save}>
		<label for="wifi-ssid">Network name (SSID)</label>
		<input id="wifi-ssid" bind:value={ssid} required disabled={loading || saving} aria-describedby="ssid-help" autocomplete="off" />
		<p id="ssid-help" class="muted">1–32 UTF-8 bytes. {ssidBytes > 32 ? 'This network name is too long.' : 'You can type it or select a scanned network below.'}</p>
		<label for="wifi-password">Network password</label>
		<input id="wifi-password" type="password" bind:value={password} disabled={loading || saving} aria-describedby="password-help" autocomplete="new-password" />
		<p id="password-help" class="muted">8–63 UTF-8 bytes for a secured network. Leave blank to keep the saved password for the same network, or to join a new open network. Passwords are never returned to this page.</p>
		<div class="wifi-actions"><button type="submit" class="buddy-button" disabled={loading || saving || scanning || ssidBytes < 1 || ssidBytes > 32 || passwordBytes > 63 || (passwordBytes > 0 && passwordBytes < 8)}>{saving ? 'Saving…' : 'Save and connect'}</button><button type="button" class="buddy-button secondary" onclick={scan} disabled={scanning || saving || loading}>{scanning ? 'Scanning…' : 'Scan nearby networks'}</button></div>
	</form>
	<section class="scan-results" aria-label="Nearby Wi-Fi networks" aria-busy={scanning}>
		<p role="status" aria-live="polite">{scanning ? 'Scanning nearby 2.4 GHz networks. You can enter a network name manually.' : scanComplete && !networks.length ? 'No nearby networks found. Enter your network name manually or scan again.' : ''}</p>
		{#if networks.length}<ul>{#each networks as network, index (`${network.ssid}-${index}`)}<li><button class="network-choice" disabled={saving || loading} onclick={() => { ssid = network.ssid; }}><strong>{network.ssid}</strong><span>{network.encryption === 0 ? 'Open network' : 'Password required'} · {network.rssi} dBm</span><span class="select-label">Select</span></button></li>{/each}</ul>{/if}
	</section>
</section>

<style>
	.wifi-setup { max-width: 760px; margin: 0 auto; padding: 28px 24px; }
	.back-link { display: inline-flex; align-items: center; min-height: 44px; margin-bottom: 12px; color: var(--accent); font-size: 13px; font-weight: 600; text-decoration: underline; text-underline-offset: 4px; border-radius: 4px; }
	.back-link:hover { color: var(--accent-strong); }
	h1 { font-size: 21px; line-height: 1.2; font-weight: 650; margin: 6px 0 8px; letter-spacing: -0.4px; }
	.setup-copy { max-width: 62ch; font-size: 14px; line-height: 1.4; color: var(--sub); }
	.connection-summary { display: flex; flex-direction: column; gap: 4px; padding: 14px 16px; margin-top: 16px; border-radius: 16px; background: var(--paper); box-shadow: var(--shadow-panel); overflow-wrap: anywhere; font-size: 14px; }
	.connection-summary span { color: var(--sub); font-size: 13px; }
	.notice { margin-top: 16px; }
	form { margin-top: 16px; padding: 16px; border-radius: 16px; background: var(--paper); box-shadow: var(--shadow-panel); }
	form > label { display: block; font-size: 13px; font-weight: 550; margin-bottom: 6px; }
	form > label:not(:first-child) { margin-top: 16px; }
	input { display: block; width: 100%; min-height: 40px; border: 1px solid var(--rule-strong); border-radius: 10px; background: #fff; color: var(--ink); padding: 8px 12px; font-size: 14px; }
	input:hover:not(:disabled) { border-color: var(--accent); }
	input:disabled { background: var(--rule); color: var(--sub); cursor: not-allowed; }
	.muted { margin-top: 6px; }
	.wifi-actions { display: flex; gap: 8px; flex-wrap: wrap; margin-top: 16px; }
	.scan-results { margin-top: 20px; font-size: 14px; line-height: 1.5; }
	.scan-results ul { list-style: none; padding: 0; margin: 12px 0 0; }
	.scan-results li + li { margin-top: 8px; }
	.network-choice { display: grid; grid-template-columns: 1fr auto; gap: 2px 16px; text-align: left; width: 100%; min-height: 56px; border: none; border-radius: 12px; padding: 10px 14px; background: var(--paper); color: var(--ink); cursor: pointer; box-shadow: var(--shadow-button); }
	.network-choice strong { overflow-wrap: anywhere; font-weight: 600; }
	.network-choice > span { color: var(--sub); font-size: 12px; }
	.network-choice > .select-label { grid-column: 2; grid-row: 1 / 3; align-self: center; color: var(--accent); font-weight: 650; }
	.network-choice:hover:not(:disabled) { background: var(--accent-soft); }
	.network-choice:disabled { opacity: 0.5; cursor: not-allowed; }
	@media (max-width: 520px) { .wifi-setup { padding: 20px 16px; } .wifi-actions { flex-direction: column; } }
</style>
