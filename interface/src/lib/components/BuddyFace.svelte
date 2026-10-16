<script lang="ts">
	let { large = false }: { large?: boolean } = $props();
</script>

<img class:large src="/arvel/character.svg" alt="" aria-hidden="true" width="170" height="170" />

<style>
	img { width: 48px; height: 48px; object-fit: contain; flex-shrink: 0; }
	img.large { width: 170px; height: 170px; }
</style>
