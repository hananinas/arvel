<script lang="ts">
	import { onMount } from 'svelte';
	import { slide } from 'svelte/transition';
	import { cubicOut } from 'svelte/easing';
	import InputPassword from '$lib/components/InputPassword.svelte';
	import SettingsCard from '$lib/components/SettingsCard.svelte';
	import { page } from '$app/state';
	import { notifications } from '$lib/components/toasts/notifications';
	import Spinner from '$lib/components/Spinner.svelte';
	import Collapsible from '$lib/components/Collapsible.svelte';
	import MQTT from '~icons/tabler/topology-star-3';
	import Client from '~icons/tabler/robot';
	import type { MQTTSettings, MQTTStatus } from '$lib/types/models';

	let mqttSettings: MQTTSettings | undefined = $state();
	let mqttStatus: MQTTStatus | undefined = $state();
	let settingsLoading = $state(true);
	let statusLoading = $state(true);

	async function getMQTTStatus() {
		try {
			const response = await fetch('/rest/mqttStatus', {
				method: 'GET',
				headers: {
					'Content-Type': 'application/json'
				}
			});
			mqttStatus = response.ok ? (await response.json()) ?? undefined : undefined;
		} catch (error) {
			mqttStatus = undefined;
			console.error('Error:', error);
		}
		statusLoading = false;
	}

	async function getMQTTSettings() {
		try {
			const response = await fetch('/rest/mqttSettings', {
				method: 'GET',
				headers: {
					'Content-Type': 'application/json'
				}
			});
			mqttSettings = response.ok ? (await response.json()) ?? undefined : undefined;
		} catch (error) {
			mqttSettings = undefined;
			console.error('Error:', error);
		}
		settingsLoading = false;
	}

	onMount(() => {
		void getMQTTStatus();
		const interval = setInterval(() => void getMQTTStatus(), 5000);
		if (!page.data.features.security) {
			void getMQTTSettings();
		}
		return () => clearInterval(interval);
	});

	let formErrors = $state({
		host: false,
		port: false,
		keep_alive: false,
		topic_length: false,
		rate_limit: false
	});

	async function postMQTTSettings(data: MQTTSettings) {
		try {
			const response = await fetch('/rest/mqttSettings', {
				method: 'POST',
				headers: {
					'Content-Type': 'application/json'
				},
				body: JSON.stringify(data)
			});
			if (response.status == 200) {
				notifications.success('MQTT settings updated.', 3000);
				mqttSettings = await response.json();
			} else {
				notifications.error('User not authorized.', 3000);
			}
		} catch (error) {
			console.error('Error:', error);
		}
		return;
	}

	function handleSubmitMQTT(event: SubmitEvent) {
		event.preventDefault();
		if (!mqttSettings) return;
		let valid = true;

		// Validate Server URI
		const regexExpURL =
			/(([-a-zA-Z0-9@:%_\+.~#?&//=]{2,256}\.[a-z]{2,4})|(\b(?:(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.){3}(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\b))(\/[-a-zA-Z0-9@:%_\+.~#?&//=]*)?/i;

		if (!regexExpURL.test(mqttSettings.uri)) {
			valid = false;
			formErrors.host = true;
		} else {
			formErrors.host = false;
		}

		// Validate if port is a number and within the right range
		let keepalive = Number(mqttSettings.keep_alive);
		if (1 <= keepalive && keepalive <= 600) {
			formErrors.keep_alive = false;
		} else {
			formErrors.keep_alive = true;
			valid = false;
		}

		// Validate it rate limit is a number and within the right range
		let ratelimit = Number(mqttSettings.message_interval_ms);
		if (0 <= ratelimit && ratelimit <= 1000) {
			formErrors.rate_limit = false;
		} else {
			formErrors.rate_limit = true;
			valid = false;
		}

		// Submit JSON to REST API
		if (valid) {
			postMQTTSettings(mqttSettings);
		}
	}

</script>

<SettingsCard collapsible={false}>
	{#snippet icon()}
		<MQTT class="lex-shrink-0 mr-2 h-6 w-6 self-end" />
	{/snippet}
	{#snippet title()}
		<span>MQTT</span>
	{/snippet}
	<div class="w-full">
		{#if statusLoading}
			<Spinner />
		{:else if mqttStatus}
			<div
				class="flex w-full flex-col space-y-1"
				transition:slide|local={{ duration: 300, easing: cubicOut }}
			>
				<div class="rounded-box bg-base-100 flex items-center space-x-3 px-4 py-2">
					<div
						class="mask mask-hexagon h-auto w-10 {mqttStatus.connected === true
							? 'bg-success'
							: 'bg-error'}"
					>
						<MQTT
							class="h-auto w-full scale-75 {mqttStatus.connected === true
								? 'text-success-content'
								: 'text-error-content'}"
						/>
					</div>
					<div>
						<div class="font-bold">Status</div>
						<div class="text-sm opacity-75">
							{#if mqttStatus.connected}
								Connected
							{:else if !mqttStatus.enabled}
								MQTT Disabled
							{:else}
								{mqttStatus.last_error}
							{/if}
						</div>
					</div>
				</div>

				<div class="rounded-box bg-base-100 flex items-center space-x-3 px-4 py-2">
					<div class="mask mask-hexagon bg-primary h-auto w-10">
						<Client class="text-primary-content h-auto w-full scale-75" />
					</div>
					<div>
						<div class="font-bold">Client ID</div>
						<div class="text-sm opacity-75">
							{mqttStatus.client_id}
						</div>
					</div>
				</div>
			</div>
		{:else}
			<p role="status">MQTT status unavailable.</p>
		{/if}
	</div>

	{#if !page.data.features.security}
		<Collapsible open={false} class="shadow-lg">
			{#snippet title()}
				<span>Change MQTT Settings</span>
			{/snippet}

			{#if settingsLoading}
				<Spinner />
			{:else if mqttSettings}
			<form
				onsubmit={handleSubmitMQTT}
				novalidate
				class="fieldset"
			>
				<div class="grid w-full grid-cols-1 content-center gap-x-4 gap-y-2 sm:grid-cols-2">
					<!-- Enable -->
					<label class="label inline-flex cursor-pointer content-end justify-start gap-4 text-base">
						<input
							type="checkbox"
							bind:checked={mqttSettings.enabled}
							class="checkbox checkbox-primary"
						/>
						Enable MQTT
					</label>

					<div class="hidden sm:block"></div>
					<!-- URI -->
					<div class="sm:col-span-2">
						<label class="label" for="host">URI</label>
						<input
							type="text"
							class="input w-full invalid:border-error invalid:border-2 {formErrors.host
								? 'border-error border-2'
								: ''}"
							bind:value={mqttSettings.uri}
							id="host"
							min="3"
							max="64"
							required
						/>
						<label class="label" for="host">
							<span class=" text-error {formErrors.host ? '' : 'hidden'}">Must be a valid URI</span>
						</label>
					</div>
					<!-- Username -->
					<div>
						<label class="label" for="user">Username </label>
						<input type="text" class="input w-full" bind:value={mqttSettings.username} id="user" />
					</div>
					<!-- Password -->
					<div>
						<label class="label" for="pwd">Password </label>
						<InputPassword bind:value={mqttSettings.password} id="pwd" />
					</div>
					<!-- Client ID -->
					<div>
						<label class="label" for="clientid">Client ID </label>
						<input
							type="text"
							class="input w-full"
							bind:value={mqttSettings.client_id}
							id="clientid"
						/>
					</div>
					<!-- Keep Alive -->
					<div>
						<label class="label" for="keepalive">Keep Alive </label>
						<label
							for="keepalive"
							class="input w-full invalid:border-error invalid:border-2 {formErrors.keep_alive
								? 'border-error border-2'
								: ''}"
						>
							<input
								type="number"
								min="1"
								max="600"
								class=""
								bind:value={mqttSettings.keep_alive}
								id="keepalive"
								required
							/>
							<span class="label">Seconds</span>
						</label>
						<label for="keepalive" class=""
							><span class=" text-error {formErrors.keep_alive ? '' : 'hidden'}"
								>Must be between 1 and 600 seconds</span
							></label
						>
					</div>
					<!-- Rate Limit -->
					<div>
						<label class="label" for="ratelimit">Publish Message Interval</label>
						<label
							for="ratelimit"
							class="input w-full invalid:border-error invalid:border-2 {formErrors.rate_limit
								? 'border-error border-2'
								: ''}"
						>
							<input
								type="number"
								min="0"
								max="1000"
								class=""
								bind:value={mqttSettings.message_interval_ms}
								id="ratelimit"
								required
							/>
							<span class="label">Milliseconds</span>
						</label>
						<label for="ratelimit" class=""
							><span class=" text-error {formErrors.rate_limit ? '' : 'hidden'}"
								>Must be between 0 and 1000 milliseconds</span
							></label
						>
					</div>
					<!-- Clean Session -->
					<label
						class="label inline-flex cursor-pointer content-end justify-start gap-4 text-base mt-2 sm:mt-4"
					>
						<input
							type="checkbox"
							bind:checked={mqttSettings.clean_session}
							class="checkbox checkbox-primary"
						/>Clean Session?
					</label>
				</div>
				<div class="divider mb-2 mt-0"></div>
				<div class="flex flex-wrap justify-end gap-2">
					<button class="btn btn-primary" type="submit">Apply Settings</button>
				</div>
			</form>
			{:else}
				<p role="status">MQTT settings unavailable.</p>
			{/if}
		</Collapsible>
	{/if}
</SettingsCard>
