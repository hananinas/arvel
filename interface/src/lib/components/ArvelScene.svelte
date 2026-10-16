<script lang="ts" module>
	const cache = new Map<string, Promise<string>>();

	function loadScene(url: string): Promise<string> {
		let entry = cache.get(url);
		if (!entry) {
			entry = fetch(url).then((response) => (response.ok ? response.text() : ''));
			entry.catch(() => cache.delete(url));
			cache.set(url, entry);
		}
		return entry;
	}
</script>

<script lang="ts">
	import { sceneById } from '$lib/arvelScenes';

	let { scene, align = 'xMidYMid slice', animate = true }: { scene: string; align?: string; animate?: boolean } = $props();
	let markup = $state('');
	let host: HTMLDivElement;
	let info = $derived(sceneById(scene));

	$effect(() => {
		const url = info.image;
		let current = true;
		loadScene(url).then((text) => {
			if (!current) return;
			// Inline the design SVG so its SMIL animations run and can be paused for reduced motion.
			markup = text
				.replace(/width="640" height="172"/, 'width="100%" height="100%"')
				.replace(/preserveAspectRatio="[^"]*"/, `preserveAspectRatio="${align}" aria-hidden="true" focusable="false"`);
		}).catch(() => { if (current) markup = ''; });
		return () => { current = false; };
	});

	$effect(() => {
		void markup;
		const svg = host?.querySelector('svg');
		if (!svg) return;
		const reduce = window.matchMedia('(prefers-reduced-motion: reduce)');
		const apply = () => (animate && !reduce.matches ? svg.unpauseAnimations() : svg.pauseAnimations());
		apply();
		reduce.addEventListener('change', apply);
		return () => reduce.removeEventListener('change', apply);
	});
</script>

<div class="scene" bind:this={host}>{@html markup}</div>

<style>
	.scene { width: 100%; height: 100%; overflow: hidden; background: #dcebe4; }
	.scene :global(svg) { display: block; width: 100%; height: 100%; }
</style>
