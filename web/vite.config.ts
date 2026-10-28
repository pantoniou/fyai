import { buildInfoPlugin } from './scripts/vite-plugin-build-info';
import { nerdamerPlugin } from './scripts/vite-plugin-nerdamer';
import { relativizeBasePlugin } from './scripts/vite-plugin-relativize-base';
import { splashScreenPlugin } from './scripts/vite-plugin-splash-screen';
import { SVELTEKIT_PWA_OPTIONS } from './src/lib/constants/pwa.constants';
import { storybookTest } from '@storybook/addon-vitest/vitest-plugin';
import { sveltekit } from '@sveltejs/kit/vite';
import tailwindcss from '@tailwindcss/vite';
import { SvelteKitPWA } from '@vite-pwa/sveltekit';
import { playwright } from '@vitest/browser-playwright';
import { dirname, resolve } from 'path';
import { fileURLToPath } from 'url';
import { defineConfig, loadEnv, searchForWorkspaceRoot } from 'vite';

const __dirname = dirname(fileURLToPath(import.meta.url));
// eslint-disable-next-line @typescript-eslint/no-explicit-any
const browserBaseConfig: any = {
	enabled: true,
	instances: [{ browser: 'chromium' }],
	provider: playwright({
		launchOptions: {
			args: ['--no-sandbox']
		}
	})
};

export default defineConfig(({ mode }) => {
	const env = loadEnv(mode, process.cwd(), 'VITE_PUBLIC_');
	/* The bridge owns every fyai process; the dev server forwards to it. */
	const BRIDGE_ORIGIN = env.VITE_PUBLIC_BRIDGE_ORIGIN || 'http://localhost:9331';

	return {
		build: {
			assetsInlineLimit: 32000,
			chunkSizeWarningLimit: 3072,
			minify: true
		},

		plugins: [
			tailwindcss(),
			sveltekit(),
			SvelteKitPWA(SVELTEKIT_PWA_OPTIONS),
			splashScreenPlugin(),
			buildInfoPlugin(),
			nerdamerPlugin(),
			relativizeBasePlugin()
		],

		resolve: {
			alias: {
				'katex-fonts': resolve('node_modules/katex/dist/fonts')
			}
		},

		server: {
			fs: {
				allow: [searchForWorkspaceRoot(process.cwd()), resolve(__dirname, 'tests')]
			},
			headers: {
				'Cross-Origin-Embedder-Policy': 'require-corp',
				'Cross-Origin-Opener-Policy': 'same-origin'
			},
			proxy: {
				'/api': BRIDGE_ORIGIN,
				'/props': BRIDGE_ORIGIN,
				'/slots': BRIDGE_ORIGIN,
				'/v1': BRIDGE_ORIGIN
			}
		},

		test: {
			projects: [
				{
					extends: './vite.config.ts',
					test: {
						browser: browserBaseConfig,
						include: ['tests/client/**/*.svelte.{test,spec}.{js,ts}'],
						name: 'client',
						setupFiles: ['./vitest-setup-client.ts']
					}
				},

				{
					extends: './vite.config.ts',
					test: {
						environment: 'node',
						// A loaded CTest run makes a cold module import take seconds.
						hookTimeout: 60000,
						include: ['tests/unit/**/*.{test,spec}.{js,ts}'],
						name: 'unit',
						testTimeout: 60000
					}
				},

				{
					extends: './vite.config.ts',
					plugins: [
						storybookTest({
							storybookScript: 'pnpm run storybook --no-open'
						})
					],
					test: {
						browser: { ...browserBaseConfig, instances: [{ browser: 'chromium', headless: true }] },
						name: 'ui'
					}
				}
			]
		}
	};
});
