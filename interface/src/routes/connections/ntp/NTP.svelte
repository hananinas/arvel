<script lang="ts">
	import { onMount } from 'svelte';
	import { slide } from 'svelte/transition';
	import { cubicOut } from 'svelte/easing';
	import SettingsCard from '$lib/components/SettingsCard.svelte';
	import Collapsible from '$lib/components/Collapsible.svelte';
	import Spinner from '$lib/components/Spinner.svelte';
	import { page } from '$app/state';
	import { notifications } from '$lib/components/toasts/notifications';
	import { TIME_ZONES } from './timezones';
	import NTP from '~icons/tabler/clock-check';
	import Server from '~icons/tabler/server';
	import Clock from '~icons/tabler/clock';
	import UTC from '~icons/tabler/clock-pin';
	import Stopwatch from '~icons/tabler/24-hours';
	import type { NTPSettings, NTPStatus } from '$lib/types/models';

	let ntpSettings: NTPSettings | undefined = $state();
	let ntpStatus: NTPStatus | undefined = $state();
	let settingsLoading = $state(true);
	let statusLoading = $state(true);

	async function getNTPStatus() {
		try {
			const response = await fetch('/rest/ntpStatus', {
				method: 'GET',
				headers: {
					'Content-Type': 'application/json'
				}
			});
			ntpStatus = response.ok ? (await response.json()) ?? undefined : undefined;
		} catch (error) {
			ntpStatus = undefined;
			console.error('Error:', error);
		}
		statusLoading = false;
	}

	async function getNTPSettings() {
		try {
			const response = await fetch('/rest/ntpSettings', {
				method: 'GET',
				headers: {
					'Content-Type': 'application/json'
				}
			});
			ntpSettings = response.ok ? (await response.json()) ?? undefined : undefined;
		} catch (error) {
			ntpSettings = undefined;
			console.error('Error:', error);
		}
		settingsLoading = false;
	}

	onMount(() => {
		void getNTPStatus();
		const interval = setInterval(() => void getNTPStatus(), 5000);
		if (!page.data.features.security) {
			void getNTPSettings();
		}
		return () => clearInterval(interval);
	});

	let formErrors = $state({
		server: false
	});

	async function postNTPSettings(data: NTPSettings) {
		try {
			const response = await fetch('/rest/ntpSettings', {
				method: 'POST',
				headers: {
					'Content-Type': 'application/json'
				},
				body: JSON.stringify(data)
			});

			if (response.status == 200) {
				notifications.success('Security settings updated.', 3000);
				ntpSettings = await response.json();
			} else {
				notifications.error('User not authorized.', 3000);
			}
		} catch (error) {
			console.error('Error:', error);
		}
	}

	function handleSubmitNTP(event: SubmitEvent) {
		event.preventDefault();
		if (!ntpSettings) return;
		let valid = true;

		// Validate Server
		// RegEx for IPv4
		const regexExpIPv4 =
			/\b(?:(?:2(?:[0-4][0-9]|5[0-5])|[0-1]?[0-9]?[0-9])\.){3}(?:(?:2([0-4][0-9]|5[0-5])|[0-1]?[0-9]?[0-9]))\b/;
		const regexExpURL =
			/[-a-zA-Z0-9@:%_\+.~#?&//=]{2,256}\.[a-z]{2,4}\b(\/[-a-zA-Z0-9@:%_\+.~#?&//=]*)?/i;

		if (!regexExpURL.test(ntpSettings.server) && !regexExpIPv4.test(ntpSettings.server)) {
			valid = false;
			formErrors.server = true;
		} else {
			formErrors.server = false;
		}

		ntpSettings.tz_format = TIME_ZONES[ntpSettings.tz_label];

		// Submit JSON to REST API
		if (valid) {
			postNTPSettings(ntpSettings);
		}
	}

	function convertSeconds(seconds: number) {
		// Calculate the number of seconds, minutes, hours, and days
		let minutes = Math.floor(seconds / 60);
		let hours = Math.floor(minutes / 60);
		let days = Math.floor(hours / 24);

		// Calculate the remaining hours, minutes, and seconds
		hours = hours % 24;
		minutes = minutes % 60;
		seconds = seconds % 60;

		// Create the formatted string
		let result = '';
		if (days > 0) {
			result += days + ' day' + (days > 1 ? 's' : '') + ' ';
		}
		if (hours > 0) {
			result += hours + ' hour' + (hours > 1 ? 's' : '') + ' ';
		}
		if (minutes > 0) {
			result += minutes + ' minute' + (minutes > 1 ? 's' : '') + ' ';
		}
		result += seconds + ' second' + (seconds > 1 ? 's' : '');

		return result;
	}

</script>

<SettingsCard collapsible={false}>
	{#snippet icon()}
		<Clock class="lex-shrink-0 mr-2 h-6 w-6 self-end" />
	{/snippet}
	{#snippet title()}
		<span>Network Time</span>
	{/snippet}
	<div class="w-full">
		{#if statusLoading}
			<Spinner />
		{:else if ntpStatus}
			<div
				class="flex w-full flex-col space-y-1"
				transition:slide|local={{ duration: 300, easing: cubicOut }}
			>
				<div class="rounded-box bg-base-100 flex items-center space-x-3 px-4 py-2">
					<div
						class="mask mask-hexagon h-auto w-10 {ntpStatus.status === 1
							? 'bg-success'
							: 'bg-error'}"
					>
						<NTP
							class="h-auto w-full scale-75 {ntpStatus.status === 1
								? 'text-success-content'
								: 'text-error-content'}"
						/>
					</div>
					<div>
						<div class="font-bold">Status</div>
						<div class="text-sm opacity-75">
							{ntpStatus.status === 1 ? 'Active' : 'Inactive'}
						</div>
					</div>
				</div>

				<div class="rounded-box bg-base-100 flex items-center space-x-3 px-4 py-2">
					<div class="mask mask-hexagon bg-primary h-auto w-10">
						<Server class="text-primary-content h-auto w-full scale-75" />
					</div>
					<div>
						<div class="font-bold">NTP Server</div>
						<div class="text-sm opacity-75">
							{ntpStatus.server}
						</div>
					</div>
				</div>

				<div class="rounded-box bg-base-100 flex items-center space-x-3 px-4 py-2">
					<div class="mask mask-hexagon bg-primary h-auto w-10">
						<Clock class="text-primary-content h-auto w-full scale-75" />
					</div>
					<div>
						<div class="font-bold">Local Time</div>
						<div class="text-sm opacity-75">
							{new Intl.DateTimeFormat('en-GB', {
								dateStyle: 'long',
								timeStyle: 'long'
							}).format(new Date(ntpStatus.local_time))}
						</div>
					</div>
				</div>

				<div class="rounded-box bg-base-100 flex items-center space-x-3 px-4 py-2">
					<div class="mask mask-hexagon bg-primary h-auto w-10">
						<UTC class="text-primary-content h-auto w-full scale-75" />
					</div>
					<div>
						<div class="font-bold">UTC Time</div>
						<div class="text-sm opacity-75">
							{new Intl.DateTimeFormat('en-GB', {
								dateStyle: 'long',
								timeStyle: 'long',
								timeZone: 'UTC'
							}).format(new Date(ntpStatus.utc_time))}
						</div>
					</div>
				</div>

				<div class="rounded-box bg-base-100 flex items-center space-x-3 px-4 py-2">
					<div class="mask mask-hexagon bg-primary h-auto w-10">
						<Stopwatch class="text-primary-content h-auto w-full scale-75" />
					</div>
					<div>
						<div class="font-bold">Uptime</div>
						<div class="text-sm opacity-75">
							{convertSeconds(ntpStatus.uptime)}
						</div>
					</div>
				</div>
			</div>
		{:else}
			<p role="status">Network time status unavailable.</p>
		{/if}
	</div>

	{#if !page.data.features.security}
		<Collapsible open={false} class="shadow-lg">
			{#snippet title()}
				<span>Change NTP Settings</span>
			{/snippet}
			{#if settingsLoading}
				<Spinner />
			{:else if ntpSettings}
			<form
				class="fieldset"
				onsubmit={handleSubmitNTP}
				novalidate
			>
				<label class="label text-base inline-flex cursor-pointer content-end justify-start gap-4">
					<input
						type="checkbox"
						bind:checked={ntpSettings.enabled}
						class="checkbox checkbox-primary"
					/>Enable NTP
				</label>

				<label class="label" for="server">Server</label>
				<input
					type="text"
					min="3"
					max="64"
					class="input w-full invalid:border-error invalid:border-2 {formErrors.server
						? 'border-error border-2'
						: ''}"
					bind:value={ntpSettings.server}
					id="server"
					required
				/>
				{#if formErrors.server}
					<p class="text-error text-sm">Please enter a valid NTP server.</p>
				{/if}

				<label class="label" for="tz">Pick Time Zone</label>
				<select class="select w-full" bind:value={ntpSettings.tz_label} id="tz">
					{#each Object.entries(TIME_ZONES) as [tz_label, tz_format]}
						<option value={tz_label}>{tz_label}</option>
					{/each}
				</select>

				<div class="mt-4 place-self-end">
					<button class="btn btn-primary" type="submit">Apply Settings</button>
				</div>
			</form>
			{:else}
				<p role="status">Network time settings unavailable.</p>
			{/if}
		</Collapsible>
	{/if}
</SettingsCard>
