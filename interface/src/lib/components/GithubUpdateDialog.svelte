<script lang="ts">
	import { modals, onBeforeClose, type ModalProps } from 'svelte-modals';
	import { focusTrap } from 'svelte-focus-trap';
	import { fly } from 'svelte/transition';
	import { telemetry } from '$lib/stores/telemetry';
	import Cancel from '~icons/tabler/x';

	interface Props extends ModalProps<void> {
		onConfirm?: () => void;
	}

	let { isOpen }: Props = $props();

	const ota = $derived($telemetry.download_ota);
	const updating = $derived(ota.status !== 'error');
	const progress = $derived(
		ota.status === 'progress' &&
			typeof ota.progress === 'number' &&
			Number.isFinite(ota.progress) &&
			ota.progress >= 0 &&
			ota.progress <= 100
			? ota.progress
			: undefined
	);
	const message = $derived(
		ota.status === 'progress'
			? 'Downloading ...'
			: ota.status === 'error'
				? ota.error || 'Firmware update failed.'
				: ota.status === 'finished'
					? 'Restarting ...'
					: 'Preparing ...'
	);

	$effect(() => {
		if (ota.status !== 'finished') return;
		// Reload after the device has had time to restart; cancel on teardown.
		const timerId = setTimeout(() => {
			modals.closeAll();
			location.reload();
		}, 20000);
		return () => clearTimeout(timerId);
	});

	onBeforeClose(() => {
		if (updating) {
			// prevents modal from closing
			return false;
		} else {
			return true;
		}
	});
</script>

{#if isOpen}
	<div
		role="dialog"
		class="pointer-events-none fixed inset-0 z-50 flex items-center justify-center"
		transition:fly={{ y: 50 }}
		use:focusTrap
	>
		<div
			class="bg-base-100 shadow-secondary/30 rounded-box pointer-events-auto flex max-h-full min-w-fit max-w-md flex-col justify-between p-4 shadow-lg"
		>
			<h2 class="text-base-content text-start text-2xl font-bold">Updating Firmware</h2>
			<div class="divider my-2"></div>
			<div class="overflow-y-auto">
				<div class="bg-base-100 flex flex-col items-center justify-center p-6">
					{#if progress !== undefined}
						<progress class="progress progress-primary w-56" value={progress} max="100"></progress>
					{:else}
						<progress class="progress progress-primary w-56"></progress>
					{/if}
					<p class="mt-8 text-2xl">{message}</p>
				</div>
			</div>
			<div class="divider my-2"></div>
			<div class="flex flex-wrap justify-end gap-2">
				<div class="grow"></div>
				<button
					class="btn btn-warning text-warning-content inline-flex flex-none items-center"
					disabled={updating}
					onclick={() => {
						modals.closeAll();
					}}
				>
					<Cancel class="mr-2 h-5 w-5" /><span>Close</span></button
				>
			</div>
		</div>
	</div>
{/if}
