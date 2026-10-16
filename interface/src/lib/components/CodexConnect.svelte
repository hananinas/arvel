<script lang="ts">
	import { onMount } from 'svelte';
	import { codexRequest, CodexRequestError, type CodexStatus } from '$lib/codex';

	let status = $state<CodexStatus | null>(null);
	let pending = $state<'status' | 'connect' | 'cancel' | 'disconnect' | null>(null);
	let error = $state('');
	let copied = $state('');
	let codeExpiry = $state(0);
	let now = $state(0);
	let alive = false;
	let controller: AbortController | undefined;
	let timer: ReturnType<typeof setTimeout> | undefined;
	let clockTimer: ReturnType<typeof setInterval> | undefined;
	let codeInput = $state<HTMLInputElement>();
	const remaining = $derived(Math.max(0, Math.ceil((codeExpiry - now) / 1000)));
	const hasCode = $derived(status?.state === 'awaiting_approval' && !!status.userCode && remaining > 0);

	function schedule() {
		clearTimeout(timer);
		if (alive && status?.busy) timer = setTimeout(() => void request('status'), 2000);
	}

	async function request(action: 'status' | 'connect' | 'cancel' | 'disconnect') {
		if (!alive || pending) return;
		clearTimeout(timer);
		if (action === 'disconnect' && !window.confirm('Remove the saved Codex sign-in on Arvel? This clears the account or an unreadable credential record, cancels pending sign-in and ends any current Codex voice session.')) {
			schedule();
			return;
		}
		pending = action;
		error = '';
		if (action !== 'status') copied = '';
		controller = new AbortController();
		const requestedAt = performance.now();
		try {
			const next = await codexRequest(action, controller.signal);
			if (!alive) return;
			if (status?.userCode !== next.userCode) copied = '';
			now = performance.now();
			// Use request-start time and subtract ceiling rounding: an offline
			// browser must not keep an expired code visible while polling fails.
			const expiry = next.state === 'awaiting_approval'
				? requestedAt + Math.max(0, next.expiresIn - 1) * 1000 : 0;
			codeExpiry = status?.userCode === next.userCode && codeExpiry > 0 ? Math.min(codeExpiry, expiry) : expiry;
			status = next;
		} catch (failure) {
			if (!alive) return;
			// A failed poll must not leave a possibly expired sign-in code visible.
			if (status) status = { ...status, userCode: '', expiresIn: 0 };
			codeExpiry = 0;
			error = failure instanceof CodexRequestError ? failure.message : 'Could not check Codex setup. Check Arvel’s connection and refresh the status.';
		} finally {
			if (alive) {
				pending = null;
				controller = undefined;
				if (!error) schedule();
			}
		}
	}

	async function copyCode() {
		if (!hasCode || !status) return;
		const code = status.userCode;
		copied = '';
		try {
			if (!navigator.clipboard?.writeText) throw new Error('Clipboard requires a secure context');
			await navigator.clipboard.writeText(code);
			if (alive && status.userCode === code) copied = 'Code copied. Enter it on OpenAI’s sign-in page.';
		} catch {
			if (!alive || status.userCode !== code || !codeInput) return;
			// The device serves HTTP; select/manual copy also works without Clipboard API access.
			codeInput.focus();
			codeInput.select();
			codeInput.setSelectionRange(0, code.length);
			let success = false;
			try { success = document.execCommand('copy'); } catch { /* Manual copy remains available. */ }
			copied = success ? 'Code copied. Enter it on OpenAI’s sign-in page.' : 'Code selected. Copy it or type it on OpenAI’s sign-in page.';
		}
	}

	function refresh() {
		if (document.visibilityState === 'visible') void request('status');
	}

	onMount(() => {
		alive = true;
		now = performance.now();
		clockTimer = setInterval(() => { now = performance.now(); }, 1000);
		void request('status');
		window.addEventListener('focus', refresh);
		document.addEventListener('visibilitychange', refresh);
		return () => {
			alive = false;
			clearTimeout(timer);
			clearInterval(clockTimer);
			controller?.abort();
			status = null;
			copied = '';
			window.removeEventListener('focus', refresh);
			document.removeEventListener('visibilitychange', refresh);
		};
	});
</script>

<section class="codex-connect" aria-labelledby="codex-account-title" data-codex-connect>
	<h3 id="codex-account-title">Connect your Codex account</h3>
	<p class="hint">Use your ChatGPT/Codex subscription. Sign in on your phone once; Arvel remembers the account across restarts. No computer needs to stay running.</p>
	{#if error}<p class="notice error" role="alert">{error}</p>{/if}
	{#if status?.error}<p class="notice error" role="alert">{status.error}</p>{/if}
	<div aria-live="polite" aria-atomic="true">
		{#if status?.configured}
			<p class="account-state"><strong>Codex account saved.</strong> Choose Codex subscription as the conversation transport and save changes, then tap the mic on Arvel to start live voice. The standalone read-aloud backend is a separate choice. Signing in here never starts recording.</p>
		{:else if status && !status.busy}
			<p class="account-state">Not signed in to Codex.</p>
		{:else if !status}
			<p class="account-state">{pending ? 'Checking the saved Codex account…' : 'Codex account status could not be checked.'}</p>
		{/if}
		{#if status?.busy && !hasCode}<p class="account-state">{status.state === 'starting' ? 'Requesting your one-time sign-in code…' : 'Waiting for OpenAI approval…'}</p>{/if}
		{#if hasCode}<p class="account-state">Your one-time code is ready. Open OpenAI sign-in below and approve this device.</p>{/if}
	</div>
	{#if hasCode && status}
		<div class="approval">
			<ol>
				<li>Open OpenAI’s sign-in page on your phone.</li>
				<li>Sign in to the subscription account you want Arvel to use.</li>
				<li>Enter this code and approve. This page checks automatically.</li>
			</ol>
			<label for="codex-device-code">One-time OpenAI code</label>
			<div class="code-row">
				<input id="codex-device-code" type="text" readonly value={status.userCode} bind:this={codeInput} autocomplete="off" autocapitalize="off" spellcheck="false" aria-describedby="codex-code-help" />
				<button class="a-btn ghost" type="button" onclick={copyCode}>Copy code</button>
			</div>
			<p class="hint" id="codex-code-help">Expires in {Math.floor(remaining / 60)}:{String(remaining % 60).padStart(2, '0')}. Only enter it at <strong>auth.openai.com</strong>; do not share it.</p>
			<a class="a-btn primary" href="https://auth.openai.com/codex/device" target="_blank" rel="noopener noreferrer">Open OpenAI sign-in ↗</a>
			<p class="hint">Opens a new tab. Device-code sign-in is beta: enable it under ChatGPT → Settings → Security if asked. A workspace administrator may need to allow it.</p>
			{#if status.configured}<p class="hint">Your previous account remains saved until this new sign-in completes. Cancelling keeps that account and does not end its voice session.</p>{/if}
		</div>
	{/if}
	<p class="hint copy-status" role="status">{copied}</p>
	<div class="actions" aria-busy={pending !== null}>
		{#if status?.busy}
			<button class="a-btn ghost" type="button" disabled={pending !== null} onclick={() => request('cancel')}>{pending === 'cancel' ? 'Cancelling…' : 'Cancel sign-in'}</button>
		{:else}
			<button class="a-btn primary" type="button" disabled={pending !== null || status === null} onclick={() => request('connect')}>{pending === 'connect' ? 'Starting sign-in…' : status?.configured ? 'Change Codex account' : 'Connect Codex'}</button>
		{/if}
		{#if status && (status.configured || status.state === 'error')}<button class="a-btn ghost" type="button" disabled={pending !== null} onclick={() => request('disconnect')}>{pending === 'disconnect' ? 'Clearing sign-in…' : status.configured ? 'Sign out of Codex' : 'Clear saved Codex sign-in'}</button>{/if}
		{#if error || !status}<button class="a-btn ghost" type="button" disabled={pending !== null} onclick={() => request('status')}>Refresh Codex status</button>{/if}
	</div>
	<p class="hint warning"><strong>Experimental direct voice.</strong> OpenAI account/workspace access and subscription limits still apply; device-code support does not guarantee live voice eligibility. Account tokens are stored in Arvel’s currently unencrypted flash. Keep the board physically trusted. This local setup page is HTTP—use trusted home Wi-Fi and never port-forward it.</p>
</section>

<style>
	.codex-connect { min-width: 0; border-top: 1px solid var(--rule-strong); margin-top: 8px; padding-top: 16px; }
	h3 { font-size: 15px; font-weight: 650; margin: 0 0 8px; }
	p { margin: 0 0 12px; overflow-wrap: anywhere; }
	.hint { font-size: 12px; line-height: 1.6; color: var(--sub); }
	.account-state { font-size: 14px; line-height: 1.6; }
	.approval { border-left: 3px solid var(--accent-strong); padding-left: 16px; margin: 16px 0; }
	ol { padding-left: 20px; margin: 0 0 16px; font-size: 14px; line-height: 1.6; }
	label { display: block; font-size: 13px; font-weight: 600; margin-bottom: 8px; }
	.code-row { display: flex; flex-wrap: wrap; align-items: center; gap: 12px; margin-bottom: 8px; }
	input { width: 220px; max-width: 100%; min-height: 44px; padding: 8px 12px; border: 1px solid var(--sub); border-radius: 8px; color: var(--ink); background: var(--paper); font-family: monospace; font-size: 20px; letter-spacing: 1px; }
	.actions { display: flex; flex-wrap: wrap; gap: 12px; margin-bottom: 16px; }
	.a-btn { min-height: 44px; white-space: normal; }
	.a-btn.primary { color: var(--paper); box-shadow: none; }
	.approval > .a-btn { margin-bottom: 12px; }
	.copy-status:empty { display: none; }
	.notice { margin-top: 12px; }
	.warning { margin-bottom: 0; }
	@media (max-width: 640px) { .actions .a-btn { flex: 1 1 160px; } }
</style>
