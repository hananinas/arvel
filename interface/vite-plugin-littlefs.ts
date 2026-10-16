import type { Plugin } from 'vite';

function withoutHash<T>(fileName: string | ((info: T) => string)) {
	const stripHash = (name: string) =>
		name.replace(/([._-])?\[hash(?::\d+)?\]/g, (_match, separator: string | undefined) =>
			separator ? '' : '[name]'
		);
	return typeof fileName === 'function'
		? (info: T) => stripHash(fileName(info))
		: stripHash(fileName);
}


export default function viteLittleFS(): Plugin[] {
	return [
		{
			name: 'vite-plugin-littlefs',
			enforce: 'post',
			apply: 'build',
			// Rollup calls this once per output, after Vite supplies its defaults.
			// This covers absent output config and output arrays without changing paths.
			outputOptions(output) {
				return {
					...output,
					assetFileNames: withoutHash(output.assetFileNames ?? '[name][extname]'),
					chunkFileNames: withoutHash(output.chunkFileNames ?? '[name].js'),
					entryFileNames: withoutHash(output.entryFileNames ?? '[name].js')
				};
			}
		}
	];
}
