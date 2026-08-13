import adapter from '@sveltejs/adapter-static';
import { vitePreprocess } from '@sveltejs/vite-plugin-svelte';

/** @type {import('@sveltejs/kit').Config} */
const config = {
	preprocess: vitePreprocess(),

	kit: {
		// No fallback: every route is prerendered, and a fallback page would
		// overwrite the prerendered index.html with an empty SPA shell,
		// dropping the title, meta tags and JSON-LD from the served markup.
		adapter: adapter()
	}
};

export default config;
