<script lang="ts">
	import { modals } from 'svelte-modals';
	import ConfirmDialog from '$lib/components/ConfirmDialog.svelte';
	import SettingsCard from '$lib/components/SettingsCard.svelte';
	import OTA from '~icons/tabler/file-upload';
	import Warning from '~icons/tabler/alert-triangle';
	import Cancel from '~icons/tabler/x';

	let files: FileList | undefined = $state();

	async function uploadBIN(file: File) {
		try {
			const formData = new FormData();
			formData.append('file', file);
			const response = await fetch('/rest/uploadFirmware', {
				method: 'POST',
				body: formData
			});
			const result = await response.json();
		} catch (error) {
			console.error('Error:', error);
		}
	}

	function confirmBinUpload() {
		const file = files?.item(0);
		if (!file) return;
		modals.open(ConfirmDialog, {
			title: 'Confirm Flashing the Device',
			message: 'Are you sure you want to overwrite the existing firmware with a new one?',
			labels: {
				cancel: { label: 'Abort', icon: Cancel },
				confirm: { label: 'Upload', icon: OTA }
			},
			onConfirm: () => {
				modals.close();
				uploadBIN(file);
			}
		});
	}
</script>

<SettingsCard collapsible={false}>
	{#snippet icon()}
		<OTA class="lex-shrink-0 mr-2 h-6 w-6 self-end rounded-full" />
	{/snippet}
	{#snippet title()}
		<span>Upload Firmware</span>
	{/snippet}
	<div class="alert alert-warning shadow-lg">
		<Warning class="h-6 w-6 shrink-0" />
		<span
			>Uploading a new firmware (.bin) file will replace the existing firmware. You may upload a
			(.md5) file first to verify the uploaded firmware.</span
		>
	</div>

	<input
		type="file"
		id="binFile"
		class="file-input file-input-secondary mt-4 w-full"
		bind:files
		accept=".bin,.md5"
		onchange={confirmBinUpload}
	/>
</SettingsCard>
