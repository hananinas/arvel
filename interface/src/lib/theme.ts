// Colour tokens from the Arvel design. Light scenes use the paper bubble; dark scenes switch to night mode.
export interface ArvelPalette {
	ink: string;
	sub: string;
	faint: string;
	accent: string;
	bubbleBg: string;
	railBg: string;
	railInk: string;
	chipBg: string;
	chipInk: string;
	userBg: string;
	userInk: string;
	rule: string;
	toggleOff: string;
	segOn: string;
	tabOnBg: string;
	tabOnInk: string;
	ringGap: string;
	activeInk: string;
}

export const MIC = '#A65B48';
export const READY = '#628B7E';
export const THINKING = '#D9A63A';

export function palette(dark: boolean): ArvelPalette {
	return dark
		? {
				ink: '#F1EFE8', sub: '#B7C0BB', faint: '#6F7B80', accent: '#8FC3BB',
				bubbleBg: 'rgba(22,28,36,0.88)', railBg: 'rgba(22,28,36,0.78)', railInk: '#E6E9E4',
				chipBg: 'rgba(242,238,234,0.14)', chipInk: '#EBD6CB',
				userBg: 'rgba(143,195,187,0.18)', userInk: '#E3EFEC',
				rule: 'rgba(255,255,255,0.09)', toggleOff: '#3A4650', segOn: '#2E3A44',
				tabOnBg: 'rgba(143,195,187,0.18)', tabOnInk: '#E3EFEC', ringGap: '#16212A', activeInk: '#16212A'
			}
		: {
				ink: '#252724', sub: '#5C6660', faint: '#A7AEA9', accent: '#456B67',
				bubbleBg: 'rgba(251,251,249,0.95)', railBg: 'rgba(251,251,249,0.9)', railInk: '#3B423C',
				chipBg: '#F2EEEA', chipInk: '#725A50',
				userBg: '#EDF4F3', userInk: '#2F4F4B',
				rule: '#E9EAE6', toggleOff: '#CDD2CD', segOn: '#FFFFFF',
				tabOnBg: '#EDF4F3', tabOnInk: '#2F4F4B', ringGap: '#FBFBF9', activeInk: '#FFFFFF'
			};
}

export function paletteStyle(p: ArvelPalette): string {
	return Object.entries(p).map(([key, value]) => `--a-${key}:${value}`).join(';');
}
