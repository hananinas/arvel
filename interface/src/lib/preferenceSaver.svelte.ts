import { saveBuddySettings, BuddySettingsRequestError, type BuddySettings, type BuddySettingsPatch, type Preferences } from '$lib/buddySettings';

export type PreferenceKey = keyof Preferences | 'readAloud' | 'scene';
export type PreferenceValues = Preferences & { readAloud: boolean; scene: string };

/**
 * Applies device preference changes optimistically, like the design's on-screen rows.
 * Steppers are debounced; failed saves revert to the device's last confirmed values.
 */
export class PreferenceSaver {
	values = $state<PreferenceValues | null>(null);
	error = $state('');
	saving = $state(false);
	private confirmed: PreferenceValues | null = null;
	private patch: BuddySettingsPatch = {};
	private timer: ReturnType<typeof setTimeout> | undefined;
	private onSaved: (settings: BuddySettings) => void;
	private paused = false;
	private generation = 0;
	private controller: AbortController | null = null;
	private inFlight: BuddySettingsPatch | null = null;

	constructor(onSaved: (settings: BuddySettings) => void = () => {}) {
		this.onSaved = onSaved;
	}

	/** Adopt values from the device unless local edits are still waiting to be saved. */
	sync(settings: { preferences: Preferences; buddy: { readAloud: boolean; scene: string } }) {
		const next = { ...settings.preferences, readAloud: settings.buddy.readAloud, scene: settings.buddy.scene };
		this.confirmed = next;
		if (!this.timer && !this.saving && !this.patch.buddy && !this.patch.preferences && JSON.stringify(next) !== JSON.stringify(this.values)) this.values = { ...next };
	}

	set(key: PreferenceKey, value: string | number | boolean, delay = 0) {
		if (!this.values || this.paused) return;
		(this.values as unknown as Record<string, unknown>)[key] = value;
		if (key === 'readAloud' || key === 'scene') this.patch.buddy = { ...this.patch.buddy, [key]: value };
		else this.patch.preferences = { ...this.patch.preferences, [key]: value };
		this.error = '';
		clearTimeout(this.timer);
		this.timer = setTimeout(() => void this.flush(), delay);
	}

	private async flush() {
		if (this.paused) return;
		this.timer = undefined;
		if (this.saving) {
			this.timer = setTimeout(() => void this.flush(), 200);
			return;
		}
		const patch = this.patch;
		this.patch = {};
		if (!patch.buddy && !patch.preferences) return;
		this.saving = true;
		const generation = this.generation;
		this.inFlight = patch;
		this.controller = new AbortController();
		try {
			const settings = await saveBuddySettings(patch, this.controller.signal);
			if (generation !== this.generation || this.paused) return;
			this.confirmed = { ...settings.preferences, readAloud: settings.buddy.readAloud, scene: settings.buddy.scene };
			if (!this.timer) this.values = { ...this.confirmed };
			this.onSaved(settings);
		} catch (failure) {
			if (generation !== this.generation || this.paused) return;
			if (failure instanceof BuddySettingsRequestError && failure.status === 401) {
				this.pause();
				return;
			}
			this.error = failure instanceof BuddySettingsRequestError && failure.status === 409
				? 'Arvel is busy, so that change wasn’t saved. Try again when it’s idle.'
				: 'That change couldn’t be saved on the device. It has been undone.';
			this.patch = {};
			clearTimeout(this.timer);
			this.timer = undefined;
			if (this.confirmed) this.values = { ...this.confirmed };
		} finally {
			if (generation === this.generation) {
				this.saving = false;
				this.inFlight = null;
				this.controller = null;
			}
		}
	}

	/** Stop requests while access is locked, retaining only non-secret preference edits. */
	pause() {
		this.paused = true;
		this.generation++;
		clearTimeout(this.timer);
		this.timer = undefined;
		if (this.inFlight?.buddy) this.patch.buddy = { ...this.inFlight.buddy, ...this.patch.buddy };
		if (this.inFlight?.preferences) this.patch.preferences = { ...this.inFlight.preferences, ...this.patch.preferences };
		this.inFlight = null;
		this.controller?.abort();
		this.controller = null;
		this.saving = false;
		this.error = '';
	}

	/** Called only after owner status and saved Settings have been confirmed. */
	resume() {
		this.paused = false;
		if (this.patch.buddy || this.patch.preferences) {
			clearTimeout(this.timer);
			this.timer = setTimeout(() => void this.flush(), 0);
		}
	}

}
