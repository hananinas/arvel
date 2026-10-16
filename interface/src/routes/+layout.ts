import type { LayoutLoad } from './$types';

// This can be false if you're using a fallback (i.e. SPA mode)
export const prerender = false;
export const ssr = false;

export const load = (async ({ fetch }) => {
	const controller = new AbortController();
	const timeout = setTimeout(() => controller.abort(), 5000);
	try {
		const result = await fetch('/rest/features', { signal: controller.signal, cache: 'no-store' });
		if (!result.ok) throw new Error('Device information is unavailable. Check your connection and reload.');
		const item = await result.json();
		return {
			features: item,
			title: 'Unravel Buddy',
			github: '',
			copyright: 'Unravel Buddy',
			appName: 'Unravel Buddy'
		};
	} finally {
		clearTimeout(timeout);
	}
}) satisfies LayoutLoad;
