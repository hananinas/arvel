// Scene ids are stored on the device; names match the design's scene picker.
export const arvelScenes = [
	{ id: 'meadow', name: 'Meadow', dark: false, image: '/arvel/meadow.svg' },
	{ id: 'night', name: 'Night', dark: true, image: '/arvel/night.svg' },
	{ id: 'workshop', name: 'Workshop', dark: false, image: '/arvel/workshop.svg' },
	{ id: 'dunes', name: 'Dunes', dark: false, image: '/arvel/dunes.svg' },
	{ id: 'rainy-window', name: 'Rainy', dark: true, image: '/arvel/rainy-window.svg' },
	{ id: 'snow', name: 'Snow', dark: false, image: '/arvel/snow.svg' },
	{ id: 'autumn', name: 'Autumn', dark: false, image: '/arvel/autumn.svg' },
	{ id: 'blossom', name: 'Blossom', dark: false, image: '/arvel/blossom.svg' },
	{ id: 'rooftop', name: 'Rooftop', dark: true, image: '/arvel/rooftop.svg' },
	{ id: 'aurora', name: 'Aurora', dark: true, image: '/arvel/aurora.svg' },
	{ id: 'library', name: 'Library', dark: false, image: '/arvel/library.svg' },
	{ id: 'campfire', name: 'Campfire', dark: true, image: '/arvel/campfire.svg' }
] as const;

export type ArvelScene = (typeof arvelScenes)[number];
export type ArvelSceneId = ArvelScene['id'];

export function sceneById(id: string | undefined): ArvelScene {
	return arvelScenes.find((scene) => scene.id === id) || arvelScenes[0];
}
