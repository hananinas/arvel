import { sveltekit } from '@sveltejs/kit/vite';
import { defineConfig, loadEnv } from 'vite';
import type { HttpProxy } from 'vite';
import Icons from 'unplugin-icons/vite';
import viteLittleFS from './vite-plugin-littlefs';
import tailwindcss from '@tailwindcss/vite';

export default defineConfig(({ mode }) => {
	const env = loadEnv(mode, process.cwd(), 'UNRAVEL_');
	const device = new URL(env.UNRAVEL_DEVICE_URL || 'http://192.168.1.22');
	if (!['http:', 'https:'].includes(device.protocol) || device.username || device.password || device.pathname !== '/' || device.search || device.hash) {
		throw new Error('UNRAVEL_DEVICE_URL must be an HTTP(S) device origin without credentials or a path.');
	}
	const restProxy = {
		target: device.origin,
		changeOrigin: true,
		configure(proxy: HttpProxy.Server) {
			proxy.on('proxyReq', (outgoing, incoming) => {
				const origin = incoming.headers.origin;
				if (!origin || origin !== `http://${incoming.headers.host}`) return;
				// Translate only same-origin loopback development requests. Keep the
				// board's mandatory Origin and owner-cookie checks intact.
				try {
					const host = new URL(origin).hostname;
					if (host === 'localhost' || host === '127.0.0.1' || host === '[::1]') outgoing.setHeader('Origin', device.origin);
				} catch {
					// Malformed client origins are not rewritten or trusted.
				}
			});
		}
	};
	return {
		plugins: [
			sveltekit(),
			Icons({
				compiler: 'svelte'
			}),
			tailwindcss(),
			// Shorten file names for LittleFS 32 char limit
			viteLittleFS()
		],
		server: {
			proxy: {
				// Browser requests stay same-origin; the board still authenticates them.
				'/rest': restProxy,
				// The legacy websocket target follows the same configured device.
				'/ws': {
					target: device.origin.replace(/^http/, 'ws'),
					changeOrigin: true,
					ws: true
				}
			}
		}
	};
});
