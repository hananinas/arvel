<script lang="ts">
	import { page } from '$app/state';
</script>

<section class="device-error" aria-labelledby="error-heading">
	<p class="eyebrow">Unravel Buddy · Device interface</p>
	<h1 id="error-heading">{page.status === 404 ? 'That page is not here.' : 'We could not open the device interface.'}</h1>
	<p>{page.status === 404 ? 'Use Conversation to return to your Buddy, or open Wi-Fi setup to reconnect the device.' : 'Check that this phone or computer is on the device’s network, then reload. Chat and Wi-Fi settings are read from the real device; there is no offline simulation.'}</p>
	<div class="error-actions"><a class="buddy-button" href="/">Open conversation</a><a class="buddy-button secondary" href="/wifi/sta">Wi-Fi setup</a><button class="buddy-button secondary" onclick={() => window.location.reload()}>Reload page</button></div>
</section>

<style>
	.device-error { max-width: 720px; margin: 0 auto; padding: 48px 24px; }
	h1 { font-size: 32px; line-height: 1.2; font-weight: 650; margin: 16px 0 24px; }
	.device-error > p { line-height: 1.6; }
	.error-actions { display: flex; flex-wrap: wrap; gap: 12px; margin-top: 32px; }
</style>
