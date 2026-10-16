import type { PageLoad } from './$types';

export const load = (async () => {
	return {
		title: 'Wi-Fi setup'
	};
}) satisfies PageLoad;
