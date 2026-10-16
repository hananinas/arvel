<script lang="ts">
	import { page } from '$app/state';
	import { Modals } from 'svelte-modals';
	import Toast from '$lib/components/toasts/Toast.svelte';
	import '../app.css';

	let { children }: { children?: import('svelte').Snippet } = $props();
	const path = $derived(page.url.pathname);
</script>

<svelte:head>
	<title>{path === '/' ? 'Arvel' : path.startsWith('/settings') ? 'Settings · Arvel' : path === '/login' ? 'Web login · Arvel' : 'Wi-Fi · Arvel'}</title>
	<meta name="theme-color" content="#EEEFEB" />
	<meta name="description" content="Arvel, the desk buddy on your ESP32 display. Real conversations and settings saved on your device." />
	<link rel="preload" href="/fonts/inter-latin.woff2" as="font" type="font/woff2" crossorigin="anonymous" />
</svelte:head>

<div class="buddy-app">
	<a class="skip-link" href="#main-content">Skip to content</a>
	<header class="app-header">
		<a class="brand" href="/" aria-label="Arvel home">
			<span class="wordmark">arvel</span>
			<span class="brand-chip">BUDDY · ESP32-S3 · 3.49″ 640×172</span>
		</a>
		<nav aria-label="Main navigation">
			<a href="/" aria-current={path === '/' ? 'page' : undefined}>Home</a>
			<a href="/settings" aria-current={path.startsWith('/settings') ? 'page' : undefined}>Settings</a>
			<a href="/wifi/sta" aria-current={path.startsWith('/wifi') ? 'page' : undefined}>Wi-Fi</a>
			<a href="/login" aria-current={path === '/login' ? 'page' : undefined}>Web login</a>
		</nav>
	</header>
	<main id="main-content" tabindex="-1">{@render children?.()}</main>
	<footer class="app-footer">Arvel is an AI assistant — answers can be mistaken, so check what matters.</footer>
</div>

<Modals>
	{#snippet backdrop({ close })}
		<button class="modal-backdrop" onclick={() => close()} aria-label="Close dialog"></button>
	{/snippet}
</Modals>
<Toast />
