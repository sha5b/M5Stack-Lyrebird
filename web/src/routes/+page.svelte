<script lang="ts">
	import { onMount } from 'svelte';
	import { base } from '$app/paths';
	import { Installer, isSupported, type DeviceInfo, type FirmwareMeta } from '$lib/installer';
	import {
		SITE_URL,
		SITE_TITLE,
		SITE_DESCRIPTION,
		SITE_KEYWORDS,
		SITE_NAME,
		ARTIST,
		PARENT_PROJECT,
		REPO_URL,
		jsonLd
	} from '$lib/seo';

	const LD_JSON = `<script type="application/ld+json">${jsonLd()}</scr` + 'ipt>';

	type Stage = 'idle' | 'connecting' | 'ready' | 'flashing' | 'done' | 'error';

	let supported = true;
	let stage: Stage = 'idle';
	let error = '';
	let errorHint = '';
	let progress = 0;
	let log: string[] = [];

	let installer = new Installer();
	let device: DeviceInfo | null = null;
	let meta: FirmwareMeta | null = null;

	$: canInstall = stage === 'ready' && meta !== null;

	onMount(async () => {
		supported = isSupported();

		try {
			// A cached manifest or image here means flashing stale firmware, so
			// bypass the HTTP cache.
			const res = await fetch(`${base}/firmware/firmware.json`, { cache: 'no-store' });
			meta = await res.json();
		} catch {
			error = 'The firmware manifest did not load.';
		}
	});

	function fail(e: unknown, fallback: string) {
		const message = e instanceof Error ? e.message : String(e);
		// The user closing the port picker is not an error worth shouting about.
		if (/No port selected|cancelled|The user aborted|NotFoundError/i.test(message)) {
			stage = device ? 'ready' : 'idle';
			return;
		}
		error = message || fallback;
		errorHint = hintFor(message);
		stage = 'error';
	}

	/**
	 * "Failed to open serial port" almost always means the OS refused access
	 * rather than anything being wrong with the board, and the fix is
	 * platform-specific — so say which one.
	 */
	function hintFor(message: string): string {
		// Board never entered the ROM loader — auto-reset did not take.
		if (
			/Failed to connect|Timed out waiting for packet|invalid head of packet|no sync|Cannot read/i.test(
				message
			)
		) {
			return (
				'The board did not start its bootloader. Power it off and on, then install ' +
				'again. A charge-only USB cable or an unpowered hub also causes this — the ' +
				'Fire needs its own cable in a real port.'
			);
		}

		const denied = /Failed to open|Access denied|NetworkError|already open|in use/i.test(message);
		if (!denied) return '';

		const ua = navigator.userAgent;
		if (/Linux|X11/.test(ua) && !/Android/.test(ua)) {
			return (
				'The dialout group owns the serial device on Linux, and your user is ' +
				'probably not a member. Run  sudo usermod -aG dialout $USER  then log out ' +
				'and log in again. A new terminal is not enough, because the browser must ' +
				'restart with the new group. Also close any serial monitor that holds the port.'
			);
		}
		if (/Mac OS X/.test(ua)) {
			return 'Close any serial monitor or IDE that holds the port. Then try again.';
		}
		return (
			'Close any serial monitor or IDE that holds the port. If the board never ' +
			'appears, install the USB-UART driver for its bridge chip (CP210x).'
		);
	}

	function addLog(line: string) {
		// esptool-js emits progress with \r; keep the tail bounded.
		const clean = line.replace(/\r/g, '').trimEnd();
		if (clean) log = [...log, clean].slice(-300);
	}

	async function connect(showAll = false) {
		error = '';
		errorHint = '';
		log = [];

		// Reuse an already-granted port so "Try again" does not re-prompt;
		// "Start over" drops the installer when a different board is wanted.
		if (!installer.hasPort() || showAll) {
			try {
				await installer.requestPort(showAll);
			} catch (e) {
				return fail(e, 'No device selected.');
			}
		}

		stage = 'connecting';
		try {
			device = await installer.connect(addLog);
			stage = 'ready';
		} catch (e) {
			await installer.disconnect();
			device = null;
			fail(e, 'The board did not answer. Unplug it, plug it in again, and retry.');
		}
	}

	async function install() {
		if (!meta) return;
		error = '';
		errorHint = '';
		stage = 'flashing';
		progress = 0;

		try {
			const res = await fetch(`${base}/firmware/${meta.image}?v=${encodeURIComponent(meta.version)}`, {
				cache: 'no-store'
			});
			const image = new Uint8Array(await res.arrayBuffer());

			await installer.flash(meta, image, (f) => (progress = f));

			stage = 'done';
		} catch (e) {
			fail(e, 'The write to flash failed.');
		} finally {
			await installer.disconnect();
		}
	}

	function startOver() {
		installer = new Installer();
		device = null;
		error = '';
		errorHint = '';
		log = [];
		progress = 0;
		stage = 'idle';
	}

	// --- Fallback path, for browsers and machines without working Web Serial ---

	$: manualCommand = meta ? `esptool.py --chip esp32 write_flash 0x0 ${meta.image}` : '';

	let copied = false;
	async function copyCommand() {
		try {
			await navigator.clipboard.writeText(manualCommand);
			copied = true;
			setTimeout(() => (copied = false), 1500);
		} catch {
			// clipboard blocked; the text is selectable anyway
		}
	}
</script>

<svelte:head>
	<title>{SITE_TITLE}</title>
	<meta name="description" content={SITE_DESCRIPTION} />
	<meta name="keywords" content={SITE_KEYWORDS} />
	<link rel="canonical" href={SITE_URL} />
	<meta property="og:type" content="website" />
	<meta property="og:site_name" content={SITE_NAME} />
	<meta property="og:title" content={SITE_TITLE} />
	<meta property="og:description" content={SITE_DESCRIPTION} />
	<meta property="og:url" content={SITE_URL} />
	<meta name="twitter:card" content="summary" />
	{@html LD_JSON}
</svelte:head>

<div class="vignette"></div>

<main>
	<header>
		<div class="header-line">
			<h1>LYREBIRD</h1>
			<span class="badge">M5STACK FIRE</span>
		</div>
		<div class="header-meta">
			<span class="meta-label">Medium</span><span class="meta-value">Physical birdsong</span>
			<span class="meta-sep">•</span>
			<span class="meta-label">Host</span><span class="meta-value">ESP32 · 240 MHz</span>
			<span class="meta-sep">•</span>
			<span class="meta-label">Species</span><span class="meta-value">12</span>
		</div>
		<p class="tagline">
			A pocket dawn chorus.<br />
			No SD card, no samples — every note is a simulated syrinx.
		</p>
		<p class="lede">
			Twelve songbird species, synthesized in real time by the Mindlin–Laje model of the
			avian vocal organ. The device boots straight into a chorus of individual birds, each
			with its own pitch and timing. Plug a M5Stack Fire in, flash it from this page.
			Ported from <a href={PARENT_PROJECT.url}>{PARENT_PROJECT.name}</a>.
		</p>
	</header>

	{#if !supported}
		<div class="notice notice-warn">
			<strong>This browser cannot reach USB devices.</strong>
			Web Serial is required. Recent Chrome, Edge, Opera and Firefox have it. Safari and
			mobile browsers do not. Use the manual route below. It writes the same image on any
			machine.
		</div>
	{:else}
		<section class="step" class:muted={stage !== 'idle' && stage !== 'connecting'}>
			<h2><span class="idx">01</span> Connect</h2>

			{#if device}
				<div class="readout">
					<div><dt>Chip</dt><dd>{device.chip}</dd></div>
					<div><dt>MAC</dt><dd>{device.mac}</dd></div>
					<div><dt>USB</dt><dd>{device.port}</dd></div>
				</div>
			{:else}
				<p class="body">
					Data USB cable, not charge-only. The picker lists CP210x, CH340 and FT232
					bridges only — the Fire's CP2104 is in the first group.
				</p>
				<div class="actions">
					<button class="cta" on:click={() => connect()} disabled={stage === 'connecting'}>
						{stage === 'connecting' ? 'Connecting…' : 'Connect device'}
					</button>
					<button class="ghost" on:click={() => connect(true)} disabled={stage === 'connecting'}>
						Show all ports
					</button>
				</div>
			{/if}
		</section>

		<section class="step" class:muted={!device}>
			<h2><span class="idx">02</span> Install</h2>

			{#if stage === 'flashing'}
				<div class="progress">
					<div class="bar"><span style="width: {Math.round(progress * 100)}%"></span></div>
					<p class="progress-label">Writing firmware — {Math.round(progress * 100)}%</p>
				</div>
			{:else if stage === 'done'}
				<div class="notice notice-ok">
					<strong>Installed.</strong>
					The board restarts on its own and boots into the chorus. If the screen stays
					dark, press the red power button once.
				</div>
				<div class="actions">
					<button class="ghost" on:click={startOver}>Flash another board</button>
				</div>
			{:else}
				<div class="actions">
					<button class="cta" on:click={install} disabled={!canInstall}>Install Lyrebird</button>
				</div>
				<p class="hint">
					Writes bootloader, partition table and firmware in one image. About a minute.
					{#if meta}<span class="version">build {meta.version}</span>{/if}
				</p>
			{/if}

			{#if error}
				<div class="notice notice-warn">
					<strong>The install did not finish.</strong>
					<p class="err-message">{error}</p>
					{#if errorHint}
						<p class="err-hint">{errorHint}</p>
					{/if}
					{#if log.length}
						<details class="logbox">
							<summary>esptool log ({log.length} lines)</summary>
							<pre>{log.join('\n')}</pre>
						</details>
					{/if}
					<div class="actions">
						<button class="cta" on:click={() => connect()}>Try again</button>
						<button class="ghost" on:click={startOver}>Start over</button>
					</div>
				</div>
			{/if}
		</section>
	{/if}

	<section class="step" id="manual">
		<h2><span class="idx">03</span> Without Web Serial</h2>
		<p class="body">
			Safari, mobile, or any browser where the picker stays empty. Same image. Needs
			<a href="https://docs.espressif.com/projects/esptool/en/latest/esp32/">esptool</a>
			(<code>pip install esptool</code>).
		</p>

		<div class="manual">
			<div class="actions">
				<a class="button-like" href="{base}/firmware/{meta?.image ?? 'lyrebird.bin'}" download>
					Download firmware
				</a>
			</div>

			<pre class="command"><code>{manualCommand}</code></pre>

			<div class="actions">
				<button class="ghost" on:click={copyCommand} disabled={!manualCommand}>
					{copied ? 'Copied' : 'Copy command'}
				</button>
			</div>

			<p class="hint">
				The file in the working directory. Add <code>--port /dev/ttyUSB0</code> (or
				<code>COM3</code>) if esptool picks the wrong one.
			</p>

			<p class="hint">
				If esptool reports a permission error on Linux, run
				<code>sudo usermod -aG dialout $USER</code>. Then log out and log in again.
			</p>
		</div>
	</section>

	<section class="step">
		<h2><span class="idx">04</span> Controls</h2>
		<p class="body">
			Three buttons on the Fire's face. Short press changes what you hear; a hold works
			the volume.
		</p>
		<div class="readout">
			<div><dt>A short</dt><dd>previous species</dd></div>
			<div><dt>C short</dt><dd>next species</dd></div>
			<div><dt>B short</dt><dd>chorus ↔ solo</dd></div>
			<div><dt>A / C hold</dt><dd>volume down / up</dd></div>
			<div><dt>B hold</dt><dd>pause / resume</dd></div>
		</div>
	</section>

	<footer>
		<div class="footer-links">
			<a href={REPO_URL}>Source &amp; manual flashing</a>
			<a href={PARENT_PROJECT.url}>{PARENT_PROJECT.name}</a>
			<a href={ARTIST.url}>{ARTIST.name}</a>
		</div>
		<p class="colophon">variable.gallery</p>
	</footer>
</main>

<style>
	.vignette {
		position: fixed;
		inset: 0;
		z-index: 1;
		pointer-events: none;
		background: radial-gradient(
			ellipse at 50% 30%,
			rgba(3, 5, 8, 0.1) 0%,
			rgba(3, 5, 8, 0.55) 55%,
			rgba(3, 5, 8, 0.88) 100%
		);
	}

	main {
		position: relative;
		z-index: 2;
		max-width: 52rem;
		margin: 0 auto;
		padding: clamp(3rem, 8vw, 7rem) var(--space-lg) var(--space-xl);
	}

	header {
		margin-bottom: clamp(3rem, 7vw, 5rem);
	}

	.header-line {
		display: flex;
		align-items: center;
		gap: var(--space-md);
	}

	h1 {
		margin: 0;
		font-size: clamp(1.6rem, 4.5vw, 2.6rem);
		font-weight: 400;
		letter-spacing: 0.02em;
		line-height: 1.05;
	}

	.badge {
		padding: 0.1rem 0.4rem;
		border: 1px solid var(--lab-accent);
		border-radius: 3px;
		font-family: var(--font-mono);
		font-size: 0.6rem;
		letter-spacing: 0.1em;
		color: var(--lab-accent);
	}

	.header-meta {
		display: flex;
		flex-wrap: wrap;
		align-items: baseline;
		gap: 0.4rem;
		margin-top: var(--space-md);
		font-family: var(--font-mono);
		font-size: 0.65rem;
		letter-spacing: 0.06em;
	}

	.meta-label {
		color: var(--lab-text-tertiary);
		text-transform: uppercase;
	}

	.meta-value {
		color: var(--lab-text-secondary);
	}

	.meta-sep {
		color: var(--lab-text-tertiary);
		padding: 0 0.3rem;
	}

	.tagline {
		margin: var(--space-xl) 0 var(--space-lg);
		font-size: clamp(1.05rem, 2.4vw, 1.5rem);
		font-weight: 300;
		line-height: 1.4;
		letter-spacing: -0.01em;
	}

	.lede {
		max-width: 52ch;
		margin: 0;
		font-size: 0.85rem;
		font-weight: 300;
		color: var(--lab-text-secondary);
	}

	.step {
		margin-bottom: clamp(2.5rem, 6vw, 4rem);
		transition: opacity var(--transition-slow);
	}

	.step.muted {
		opacity: 0.35;
	}

	h2 {
		display: flex;
		align-items: baseline;
		gap: var(--space-md);
		margin: 0 0 var(--space-lg);
		font-size: 0.95rem;
		font-weight: 500;
		letter-spacing: 0.02em;
	}

	.idx {
		font-family: var(--font-mono);
		font-size: 0.7rem;
		color: var(--lab-text-tertiary);
	}

	.body {
		max-width: 58ch;
		margin: 0 0 var(--space-lg);
		font-size: 0.85rem;
		font-weight: 300;
		color: var(--lab-text-secondary);
	}

	.actions {
		display: flex;
		flex-wrap: wrap;
		gap: var(--space-sm);
		align-items: center;
	}

	.cta {
		padding: 0.7rem 1.6rem;
		font-size: 0.85rem;
		border-color: var(--lab-accent);
		background: var(--lab-accent-dim);
	}

	.cta:hover:not(:disabled) {
		background: var(--lab-accent);
		border-color: var(--lab-accent);
		color: var(--lab-bg);
	}

	.cta:disabled,
	.ghost:disabled {
		opacity: 0.35;
		cursor: default;
	}

	.ghost {
		background: transparent;
		font-size: 0.78rem;
		color: var(--lab-text-secondary);
	}

	.readout {
		display: grid;
		gap: 0.35rem;
		padding: var(--space-lg);
		border: 1px solid var(--lab-border);
		border-radius: 10px;
		background: var(--lab-glass);
		backdrop-filter: blur(14px);
	}

	.readout div {
		display: flex;
		justify-content: space-between;
		gap: var(--space-lg);
		font-size: 0.75rem;
	}

	.readout dt {
		color: var(--lab-text-tertiary);
	}

	.readout dd {
		margin: 0;
		font-family: var(--font-mono);
		color: var(--lab-text-primary);
		text-align: right;
		word-break: break-all;
	}

	.progress {
		padding: var(--space-lg);
		border: 1px solid var(--lab-border);
		border-radius: 10px;
		background: var(--lab-glass);
		backdrop-filter: blur(14px);
	}

	.bar {
		height: 3px;
		border-radius: 2px;
		background: var(--lab-surface);
		overflow: hidden;
	}

	.bar span {
		display: block;
		height: 100%;
		background: var(--lab-accent);
		transition: width var(--transition-normal);
	}

	.progress-label {
		margin: var(--space-md) 0 0;
		font-family: var(--font-mono);
		font-size: 0.72rem;
		color: var(--lab-text-secondary);
	}

	.notice {
		padding: var(--space-lg);
		border-radius: 10px;
		font-size: 0.82rem;
		font-weight: 300;
		line-height: 1.7;
		color: var(--lab-text-secondary);
		border: 1px solid var(--lab-border);
		background: var(--lab-glass);
		backdrop-filter: blur(14px);
	}

	.notice + .actions,
	.notice .actions {
		margin-top: var(--space-md);
	}

	.notice-warn {
		border-color: rgba(245, 101, 101, 0.3);
	}

	.notice-ok {
		border-color: var(--lab-accent);
	}

	.notice strong {
		display: block;
		color: var(--lab-text-primary);
		font-weight: 500;
	}

	.logbox {
		margin-top: var(--space-md);
	}

	.logbox summary {
		font-size: 0.72rem;
		color: var(--lab-text-tertiary);
		cursor: pointer;
		padding: var(--space-sm) 0;
	}

	.logbox summary:hover {
		color: var(--lab-accent);
	}

	.logbox pre {
		max-height: 14rem;
		margin: var(--space-sm) 0 0;
		padding: var(--space-md);
		overflow: auto;
		border: 1px solid var(--lab-border);
		border-radius: 6px;
		background: var(--lab-bg);
		font-family: var(--font-mono);
		font-size: 0.68rem;
		line-height: 1.6;
		color: var(--lab-text-secondary);
		user-select: text;
	}

	.manual {
		padding: var(--space-lg);
		border: 1px solid var(--lab-border);
		border-radius: 10px;
		background: var(--lab-glass);
		backdrop-filter: blur(14px);
	}

	.command {
		margin: var(--space-lg) 0 0;
		padding: var(--space-md);
		border: 1px solid var(--lab-border);
		border-radius: 6px;
		background: var(--lab-bg);
		overflow-x: auto;
	}

	.command code {
		padding: 0;
		background: none;
		font-size: 0.72rem;
		line-height: 1.7;
		color: var(--lab-text-primary);
		white-space: pre;
		user-select: text;
	}

	.manual .actions + .command,
	.manual .command + .actions {
		margin-top: var(--space-md);
	}

	.manual .hint code {
		user-select: text;
	}

	.err-message {
		margin: var(--space-sm) 0 0;
		font-family: var(--font-mono);
		font-size: 0.75rem;
		color: var(--lab-danger);
		user-select: text;
	}

	.err-hint {
		margin: var(--space-md) 0 0;
		padding-top: var(--space-md);
		border-top: 1px solid var(--lab-border);
		user-select: text;
	}

	.hint {
		margin: var(--space-md) 0 0;
		font-size: 0.75rem;
		color: var(--lab-text-tertiary);
	}

	.version {
		font-family: var(--font-mono);
		margin-left: var(--space-sm);
	}

	footer {
		display: flex;
		flex-wrap: wrap;
		gap: var(--space-md) var(--space-lg);
		align-items: center;
		justify-content: space-between;
		margin-top: clamp(3rem, 8vw, 5rem);
		padding-top: var(--space-lg);
		border-top: 1px solid var(--lab-border);
	}

	.footer-links {
		display: flex;
		flex-wrap: wrap;
		gap: var(--space-lg);
		font-size: 0.75rem;
	}

	.footer-links a {
		color: var(--lab-text-secondary);
	}

	.footer-links a:hover {
		color: var(--lab-accent);
	}

	.colophon {
		margin: 0;
		font-family: var(--font-mono);
		font-size: 0.68rem;
		color: var(--lab-text-tertiary);
	}
</style>
