<script lang="ts">
	import { onMount, tick, untrack, type Snippet } from 'svelte';
	import { ownerAccess, checkOwnerAccess, authenticateOwner, logoutOwner, OwnerAccessError } from '$lib/ownerAccess';

	let { children, onauthenticated = () => {}, onlocked = () => {}, onclearsecrets = () => {} }: {
		children?: Snippet;
		onauthenticated?: () => void;
		onlocked?: () => void;
		onclearsecrets?: () => void;
	} = $props();
	let ready = $state(false);
	let mode = $state<'login' | 'setup'>('login');
	let username = $state('');
	let password = $state('');
	let confirmation = $state('');
	let code = $state('');
	let remember = $state(true);
	let showPassword = $state(false);
	let pending = $state(false);
	let error = $state('');
	let cooldown = $state(0);
	let errorElement = $state<HTMLDivElement>();
	let contentElement = $state<HTMLDivElement>();
	let usernameElement = $state<HTMLInputElement>();
	let wasAuthenticated = false;
	let alive = false;
	let timer: ReturnType<typeof setInterval> | undefined;
	const authenticated = $derived(ready && $ownerAccess.status?.authenticated === true);

	function clearSecrets() {
		password = '';
		confirmation = '';
		code = '';
		showPassword = false;
	}

	$effect(() => {
		const next = authenticated;
		if (next === wasAuthenticated) return;
		wasAuthenticated = next;
		untrack(() => {
			clearSecrets();
			if (next) {
				mode = 'login';
				onauthenticated();
				void tick().then(() => { if (alive) contentElement?.focus(); });
			} else {
				onlocked();
				void tick().then(() => { if (alive) (errorElement || usernameElement)?.focus(); });
			}
		});
	});

	async function refresh() {
		try {
			const status = await checkOwnerAccess();
			if (alive && !status.configured) mode = 'setup';
		} catch { clearSecrets(); /* The helper exposes a safe, actionable status error. */ }
		finally { if (alive) ready = true; }
	}

	async function chooseMode(next: 'login' | 'setup') {
		mode = next;
		error = '';
		clearSecrets();
		await tick();
		usernameElement?.focus();
		void refresh();
	}

	async function focusError() {
		await tick();
		if (alive) errorElement?.focus();
	}

	async function submit(event: SubmitEvent) {
		event.preventDefault();
		if (pending || cooldown || !$ownerAccess.status) return;
		error = '';
		const bytes = new TextEncoder().encode(password).length;
		if (!/^[A-Za-z0-9._-]{1,32}$/.test(username)) error = 'Enter a username of 1–32 letters, numbers, dots, underscores or hyphens.';
		else if (bytes < 8 || bytes > 128 || /[\u0000-\u001f\u007f-\u009f]/.test(password)) error = 'Use a password or passphrase of 8–128 UTF-8 bytes, without control characters. Spaces are allowed.';
		else if (mode === 'setup' && password !== confirmation) error = 'The passwords do not match. Enter the same password in both fields.';
		else if (mode === 'setup' && !/^\d{6}$/.test(code)) error = 'Enter the six-digit Web login approval code displayed on Arvel.';
		if (error) { await focusError(); return; }
		pending = true;
		try {
			await authenticateOwner(mode, { username, password, remember, ...(mode === 'setup' ? { code } : {}) });
		} catch (failure) {
			if (!alive) return;
			error = failure instanceof OwnerAccessError ? failure.message : 'Login could not be confirmed. Check Arvel and retry.';
			if (failure instanceof OwnerAccessError && failure.retryAfter) {
				cooldown = Math.ceil(failure.retryAfter);
				clearInterval(timer);
				timer = setInterval(() => { cooldown = Math.max(0, cooldown - 1); if (!cooldown) clearInterval(timer); }, 1000);
			}
			await focusError();
		} finally {
			clearSecrets();
			if (alive) pending = false;
		}
	}

	async function logout() {
		if (pending) return;
		pending = true;
		error = '';
		onclearsecrets();
		try { await logoutOwner(); }
		catch (failure) {
			error = failure instanceof OwnerAccessError ? failure.message : 'Logout could not be confirmed. Retry to revoke this browser’s access.';
			await focusError();
		} finally {
			onclearsecrets();
			if (alive) pending = false;
		}
	}

	onMount(() => {
		alive = true;
		void refresh();
		const onFocus = () => { if (!pending) void refresh(); };
		const onVisibility = () => { if (document.visibilityState === 'visible') onFocus(); };
		window.addEventListener('focus', onFocus);
		document.addEventListener('visibilitychange', onVisibility);
		return () => {
			alive = false;
			clearSecrets();
			clearInterval(timer);
			window.removeEventListener('focus', onFocus);
			document.removeEventListener('visibilitychange', onVisibility);
		};
	});
</script>

<div class="owner-access" data-owner-access>
	{#if error || $ownerAccess.error}
		<div class="notice error" role="alert" tabindex="-1" bind:this={errorElement} data-owner-error>{error || $ownerAccess.error}</div>
	{/if}
	{#if !ready || (!$ownerAccess.status && $ownerAccess.checking)}
		<p role="status">Checking web access on Arvel…</p>
	{:else if !$ownerAccess.status}
		<button class="a-btn primary" type="button" disabled={$ownerAccess.checking} onclick={refresh}>Retry checking web access</button>
	{:else if authenticated}
		<div class="session-bar">
			<p>Signed in as <strong>{$ownerAccess.status.username || 'owner'}</strong></p>
			<button class="a-btn ghost" type="button" disabled={pending} onclick={logout} data-owner-logout>{pending ? 'Logging out…' : 'Log out'}</button>
		</div>
		<div tabindex="-1" bind:this={contentElement} data-owner-content>{@render children?.()}</div>
	{:else}
		<section class="login-card" aria-labelledby="owner-access-title">
			<h2 id="owner-access-title">{mode === 'login' ? 'Log in to Arvel' : $ownerAccess.status.configured ? 'Recover web login' : 'Create your web login'}</h2>
			{#if mode === 'setup'}
				<p>On Arvel’s screen, open <strong>Settings &gt; Device &gt; Web login</strong>. Enter the six-digit code shown there, then choose your username and password. The code lasts five minutes. You do not need to switch Wi-Fi networks.</p>
				{#if $ownerAccess.status.configured}<p>Saving replaces the existing owner login and signs out every browser. Saved API keys and the connected Codex account are retained; any pending Codex sign-in is cancelled.</p>{/if}
				<p class="approval-state">{$ownerAccess.status.approvalOpen ? 'A physical approval window is open.' : 'Generate a new code on the device before submitting.'}</p>
			{:else}
				<p>Use the owner login you created on Arvel to unlock its device state preview, native conversation, remote controls and Settings.</p>
			{/if}
			{#if !$ownerAccess.status.storageAvailable}<p class="notice error">Web-login storage needs attention. Try physically approved creation or recovery to replace the stored login; access stays locked until Arvel confirms it was saved.</p>{/if}
			<form onsubmit={submit} novalidate aria-label={mode === 'login' ? 'Owner login' : 'Physically approved owner login'} aria-busy={pending}>
				<fieldset disabled={pending}>
					<div class="field">
						<label for="owner-username">Username</label>
						<input id="owner-username" name="username" type="text" autocomplete="username" autocapitalize="off" spellcheck="false" maxlength="32" required bind:value={username} bind:this={usernameElement} aria-describedby="owner-username-help" />
						<p class="hint" id="owner-username-help">1–32 letters, numbers, dots, underscores or hyphens.</p>
					</div>
					<div class="field">
						<label for="owner-password">{mode === 'setup' ? 'New password' : 'Password'}</label>
						<input id="owner-password" name="password" type={showPassword ? 'text' : 'password'} autocomplete={mode === 'setup' ? 'new-password' : 'current-password'} required bind:value={password} aria-describedby="owner-password-help" />
						<p class="hint" id="owner-password-help">8–128 UTF-8 bytes. A passphrase with spaces is welcome.</p>
					</div>
					{#if mode === 'setup'}
						<div class="field"><label for="owner-password-confirm">Confirm new password</label><input id="owner-password-confirm" name="password-confirm" type={showPassword ? 'text' : 'password'} autocomplete="new-password" required bind:value={confirmation} /></div>
						<div class="field"><label for="owner-code">Web login approval code</label><input id="owner-code" name="code" type="text" inputmode="numeric" autocomplete="one-time-code" pattern="[0-9]{6}" maxlength="6" required bind:value={code} aria-describedby="owner-code-help" /><p class="hint" id="owner-code-help">Six digits from Web login on Arvel, not the Wi-Fi password.</p></div>
					{/if}
					<label class="check"><input type="checkbox" bind:checked={showPassword} /><span>Show {mode === 'setup' ? 'passwords' : 'password'}</span></label>
					<label class="check"><input id="owner-remember" name="remember" type="checkbox" bind:checked={remember} /><span>Remember this browser</span></label>
					<p class="hint">Use only on your own browser. Remembered access survives device restarts until logout, login recovery or replacement by another browser session. Unchecked access lasts up to eight hours and ends on device restart.</p>
					<button class="a-btn primary" type="submit" disabled={cooldown > 0} data-owner-submit>{pending ? 'Confirming on Arvel…' : cooldown ? `Try again in ${cooldown}s` : mode === 'login' ? 'Log in' : $ownerAccess.status.configured ? 'Replace web login' : 'Create web login'}</button>
				</fieldset>
			</form>
			<div class="mode-actions">
				{#if mode === 'login'}<button class="a-btn ghost" type="button" disabled={pending} onclick={() => chooseMode('setup')}>Forgot password?</button>
				{:else if $ownerAccess.status.configured}<button class="a-btn ghost" type="button" disabled={pending} onclick={() => chooseMode('login')}>Back to login</button>{/if}
				<button class="a-btn ghost" type="button" disabled={pending || $ownerAccess.checking} onclick={refresh}>Check device approval</button>
			</div>
		</section>
	{/if}
	<p class="http-warning"><strong>Trusted home Wi-Fi only.</strong> This page uses unencrypted HTTP, including login, entered keys and one-time sign-in codes. Others on the network may capture access. Never port-forward Arvel. Provider and direct Codex signaling connections still require verified HTTPS.</p>
</div>

<style>
	.owner-access { isolation: isolate; min-width: 0; }
	.login-card { max-width: 640px; background: var(--paper); border: 1px solid var(--rule-strong); border-radius: 16px; padding: 24px; }
	h2 { font-size: 20px; font-weight: 650; line-height: 1.2; margin: 0 0 16px; }
	p { font-size: 14px; line-height: 1.6; margin: 0 0 16px; max-width: 70ch; overflow-wrap: anywhere; }
	fieldset { border: 0; padding: 0; margin: 24px 0 0; min-width: 0; }
	.field { display: flex; flex-direction: column; gap: 8px; margin-bottom: 16px; }
	.field label { font-size: 14px; font-weight: 600; }
	.field input { width: 100%; min-height: 44px; border: 1px solid var(--sub); border-radius: 8px; padding: 8px 12px; background: var(--paper); color: var(--ink); }
	.field input:hover:not(:disabled) { border-color: var(--accent-strong); }
	.hint { color: var(--sub); font-size: 12px; margin: 0; line-height: 1.6; }
	.check { display: flex; align-items: center; gap: 12px; min-height: 44px; font-size: 14px; cursor: pointer; }
	.check input { width: 20px; height: 20px; accent-color: var(--accent); flex-shrink: 0; }
	fieldset > .hint { margin: 8px 0 24px; }
	.mode-actions, .session-bar { display: flex; flex-wrap: wrap; align-items: center; gap: 12px; }
	.mode-actions { margin-top: 16px; }
	.session-bar { justify-content: space-between; margin-bottom: 16px; }
	.session-bar p { margin: 0; }
	.a-btn { min-height: 44px; white-space: normal; }
	.a-btn:active:not(:disabled) { background: var(--rule-strong); color: var(--ink); }
	.a-btn.primary { color: var(--paper); box-shadow: none; }
	.notice { margin-bottom: 16px; }
	.approval-state { color: var(--accent-strong); font-weight: 600; }
	.http-warning { color: var(--sub); font-size: 12px; margin-top: 24px; }
	@media (max-width: 640px) { .login-card { padding: 16px; } .mode-actions { align-items: stretch; } .mode-actions .a-btn { flex: 1 1 200px; } }
</style>
