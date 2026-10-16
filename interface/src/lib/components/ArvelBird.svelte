<script lang="ts">
	import { arvelBirdMarkup } from '$lib/arvelBird';

	let { bobDur = '3.2s', size = 170, animate = true }: { bobDur?: string; size?: number; animate?: boolean } = $props();
	let svg: SVGSVGElement;

	$effect(() => {
		void bobDur;
		const reduce = window.matchMedia('(prefers-reduced-motion: reduce)');
		const apply = () => (animate && !reduce.matches ? svg?.unpauseAnimations() : svg?.pauseAnimations());
		apply();
		reduce.addEventListener('change', apply);
		return () => reduce.removeEventListener('change', apply);
	});
</script>

<svg bind:this={svg} width={size} height={size} viewBox="0 0 80 80" role="img" aria-label="Arvel">
	{#key bobDur}
		<g>
			<animateTransform attributeName="transform" type="translate" values="0 0;0 -1.3;0 0" dur={bobDur} repeatCount="indefinite" />
			{@html arvelBirdMarkup}
		</g>
	{/key}
</svg>

<style>
	svg { display: block; }
</style>
