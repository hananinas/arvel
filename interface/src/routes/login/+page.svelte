<script lang="ts">
	import OwnerAccess from '$lib/components/OwnerAccess.svelte';
	import { page } from '$app/state';
	import { goto } from '$app/navigation';

	let returnHome = $derived(page.url.searchParams.get('returnTo') === 'home');

	function authenticated() {
		if (returnHome) void goto('/');
	}
</script>

<svelte:head>
	<title>Web login · Arvel</title>
	<meta name="description" content="Log in to view Arvel’s native device state and control it, or physically approve creating and recovering your owner login." />
</svelte:head>

<section class="login-page" aria-labelledby="login-title">
	<header><h1 id="login-title">Web login</h1><p>One owner login for Arvel’s device state preview, native conversation, remote controls and Settings.</p></header>
	<OwnerAccess onauthenticated={authenticated}>
		<div class="signed-in"><h2>Owner access confirmed</h2><p>Your browser’s access is confirmed by physical Arvel.</p><a class="a-btn primary" href="/">Return Home</a> <a class="a-btn ghost" href="/settings">Open Settings</a></div>
	</OwnerAccess>
</section>

<style>
	.login-page { max-width: 800px; padding: 32px; margin: 0 auto; }
	header { margin-bottom: 24px; }
	h1 { font-size: 24px; font-weight: 650; line-height: 1.2; margin-bottom: 8px; }
	h2 { font-size: 20px; font-weight: 650; margin-bottom: 8px; }
	p { font-size: 14px; line-height: 1.6; color: var(--sub); }
	.signed-in { background: var(--paper); border: 1px solid var(--rule-strong); border-radius: 16px; padding: 24px; }
	.signed-in .a-btn { margin-top: 16px; min-height: 44px; }
	@media (max-width: 640px) { .login-page { padding: 24px 16px; } }
</style>
