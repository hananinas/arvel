<script lang="ts">
	import SettingsCard from '$lib/components/SettingsCard.svelte';
	import { slide } from 'svelte/transition';
	import { cubicOut } from 'svelte/easing';
	import { Chart, registerables } from 'chart.js';
	import Metrics from '~icons/tabler/report-analytics';
	import { daisyColor } from '$lib/DaisyUiHelper';
	import { analytics } from '$lib/stores/analytics';
	import type { Analytics } from '$lib/types/models';

	Chart.register(...registerables);

	let hasPsramData = $derived($analytics.psram_size.some((size) => size > 0));

	function metricChart(
		element: HTMLCanvasElement,
		title: string,
		series: { label: string; key: keyof Analytics; color: string }[],
		maximum?: keyof Analytics
	) {
		const chart = new Chart<'line', number[], number>(element, {
			type: 'line',
			data: {
				labels: [],
				datasets: series.map(({ label, color }) => ({
					label,
					borderColor: daisyColor(color),
					backgroundColor: daisyColor(color, 50),
					borderWidth: 2,
					data: [],
					yAxisID: 'y'
				}))
			},
			options: {
				maintainAspectRatio: false,
				responsive: true,
				plugins: {
					legend: { display: true },
					tooltip: { mode: 'index', intersect: false }
				},
				elements: { point: { radius: 1 } },
				scales: {
					x: {
						grid: { color: daisyColor('--color-base-content', 10) },
						ticks: { color: daisyColor('--color-base-content') },
						display: false
					},
					y: {
						type: 'linear',
						title: {
							display: true,
							text: title,
							color: daisyColor('--color-base-content'),
							font: { size: 16, weight: 'bold' }
						},
						position: 'left',
						min: maximum ? 0 : undefined,
						suggestedMin: maximum ? undefined : 20,
						suggestedMax: maximum ? undefined : 100,
						grid: { color: daisyColor('--color-base-content', 10) },
						ticks: { color: daisyColor('--color-base-content') },
						border: { color: daisyColor('--color-base-content', 10) }
					}
				}
			}
		});

		const unsubscribe = analytics.subscribe((history) => {
			chart.data.labels = history.uptime;
			series.forEach(({ key }, index) => {
				chart.data.datasets[index].data = history[key];
			});
			const y = chart.options.scales?.y;
			if (maximum && y) {
				const capacities = history[maximum];
				y.max = capacities.length > 0 ? Math.max(...capacities) : undefined;
			}
			chart.update('none');
		});

		return {
			destroy() {
				unsubscribe();
				chart.destroy();
			}
		};
	}

	function heapMetrics(element: HTMLCanvasElement) {
		return metricChart(element, 'Memory [KB]', [
			{ label: 'Used', key: 'used_heap', color: '--color-primary' },
			{ label: 'Max Alloc', key: 'max_alloc_heap', color: '--color-secondary' }
		], 'total_heap');
	}

	function psramMetrics(element: HTMLCanvasElement) {
		return metricChart(element, 'PSRAM [KB]', [
			{ label: 'Used', key: 'used_psram', color: '--color-primary' }
		], 'psram_size');
	}

	function filesystemMetrics(element: HTMLCanvasElement) {
		return metricChart(element, 'File System [KB]', [
			{ label: 'Used', key: 'fs_used', color: '--color-primary' }
		], 'fs_total');
	}

	function temperatureMetrics(element: HTMLCanvasElement) {
		return metricChart(element, 'Core Temperature [°C]', [
			{ label: 'Core Temperature', key: 'core_temp', color: '--color-primary' }
		]);
	}
</script>

<SettingsCard collapsible={false}>
	{#snippet icon()}
		<Metrics class="lex-shrink-0 mr-2 h-6 w-6 self-end" />
	{/snippet}
	{#snippet title()}
		<span>System Metrics</span>
	{/snippet}

	{#if $analytics.uptime.length === 0}
		<p role="status">Waiting for system metrics…</p>
	{/if}
	<div class="w-full overflow-x-auto">
		<div
			class="flex w-full flex-col space-y-1 h-60"
			transition:slide|local={{ duration: 300, easing: cubicOut }}
		>
			<canvas use:heapMetrics></canvas>
		</div>
	</div>
	{#if hasPsramData}
		<div class="w-full overflow-x-auto">
			<div
				class="flex w-full flex-col space-y-1 h-60"
				transition:slide|local={{ duration: 300, easing: cubicOut }}
			>
				<canvas use:psramMetrics></canvas>
			</div>
		</div>
	{/if}
	<div class="w-full overflow-x-auto">
		<div
			class="flex w-full flex-col space-y-1 h-52"
			transition:slide|local={{ duration: 300, easing: cubicOut }}
		>
			<canvas use:filesystemMetrics></canvas>
		</div>
	</div>
	<div class="w-full overflow-x-auto">
		<div
			class="flex w-full flex-col space-y-1 h-52"
			transition:slide|local={{ duration: 300, easing: cubicOut }}
		>
			<canvas use:temperatureMetrics></canvas>
		</div>
	</div>
</SettingsCard>
