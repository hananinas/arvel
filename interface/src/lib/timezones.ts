// POSIX TZ rules the device can apply with setenv("TZ"). `zones` are IANA names the browser may report.
export const timezones: { label: string; tz: string; zones: string[] }[] = [
	{ label: 'UTC', tz: 'UTC0', zones: ['UTC', 'Etc/UTC', 'Etc/GMT'] },
	{ label: 'London, Dublin, Lisbon', tz: 'GMT0BST,M3.5.0/1,M10.5.0', zones: ['Europe/London', 'Europe/Dublin', 'Europe/Lisbon'] },
	{ label: 'Copenhagen, Berlin, Paris, Stockholm', tz: 'CET-1CEST,M3.5.0,M10.5.0/3', zones: ['Europe/Copenhagen', 'Europe/Berlin', 'Europe/Paris', 'Europe/Stockholm', 'Europe/Oslo', 'Europe/Amsterdam', 'Europe/Brussels', 'Europe/Madrid', 'Europe/Rome', 'Europe/Vienna', 'Europe/Zurich', 'Europe/Prague', 'Europe/Warsaw', 'Europe/Budapest'] },
	{ label: 'Helsinki, Athens, Kyiv', tz: 'EET-2EEST,M3.5.0/3,M10.5.0/4', zones: ['Europe/Helsinki', 'Europe/Athens', 'Europe/Kyiv', 'Europe/Kiev', 'Europe/Riga', 'Europe/Tallinn', 'Europe/Vilnius', 'Europe/Bucharest', 'Europe/Sofia'] },
	{ label: 'Istanbul, Moscow', tz: '<+03>-3', zones: ['Europe/Istanbul', 'Europe/Moscow'] },
	{ label: 'Dubai', tz: '<+04>-4', zones: ['Asia/Dubai'] },
	{ label: 'India', tz: 'IST-5:30', zones: ['Asia/Kolkata', 'Asia/Calcutta'] },
	{ label: 'Bangkok, Jakarta', tz: '<+07>-7', zones: ['Asia/Bangkok', 'Asia/Jakarta', 'Asia/Ho_Chi_Minh'] },
	{ label: 'Singapore, Beijing, Perth', tz: '<+08>-8', zones: ['Asia/Singapore', 'Asia/Shanghai', 'Asia/Hong_Kong', 'Asia/Taipei', 'Australia/Perth', 'Asia/Kuala_Lumpur', 'Asia/Manila'] },
	{ label: 'Tokyo, Seoul', tz: 'JST-9', zones: ['Asia/Tokyo', 'Asia/Seoul'] },
	{ label: 'Sydney, Melbourne', tz: 'AEST-10AEDT,M10.1.0,M4.1.0/3', zones: ['Australia/Sydney', 'Australia/Melbourne', 'Australia/Canberra', 'Australia/Hobart'] },
	{ label: 'Auckland', tz: 'NZST-12NZDT,M9.5.0,M4.1.0/3', zones: ['Pacific/Auckland'] },
	{ label: 'São Paulo', tz: '<-03>3', zones: ['America/Sao_Paulo', 'America/Argentina/Buenos_Aires'] },
	{ label: 'US Eastern (New York, Toronto)', tz: 'EST5EDT,M3.2.0,M11.1.0', zones: ['America/New_York', 'America/Toronto', 'America/Detroit'] },
	{ label: 'US Central (Chicago)', tz: 'CST6CDT,M3.2.0,M11.1.0', zones: ['America/Chicago', 'America/Winnipeg'] },
	{ label: 'US Mountain (Denver)', tz: 'MST7MDT,M3.2.0,M11.1.0', zones: ['America/Denver', 'America/Edmonton'] },
	{ label: 'Arizona (Phoenix)', tz: 'MST7', zones: ['America/Phoenix'] },
	{ label: 'US Pacific (Los Angeles, Vancouver)', tz: 'PST8PDT,M3.2.0,M11.1.0', zones: ['America/Los_Angeles', 'America/Vancouver'] },
	{ label: 'Alaska', tz: 'AKST9AKDT,M3.2.0,M11.1.0', zones: ['America/Anchorage'] },
	{ label: 'Hawaii', tz: 'HST10', zones: ['Pacific/Honolulu'] }
];

export function browserTimezone(): string | null {
	try {
		const zone = Intl.DateTimeFormat().resolvedOptions().timeZone;
		return timezones.find((entry) => entry.zones.includes(zone))?.tz ?? null;
	} catch {
		return null;
	}
}

export function timezoneLabel(tz: string): string {
	return timezones.find((entry) => entry.tz === tz)?.label ?? tz;
}
