<script lang="ts">
	import { onMount, onDestroy } from 'svelte';
	import { base } from '$app/paths';
	import { createGalaxy, type GalaxyHandle } from '$lib/galaxy';
	import {
		Installer,
		isSupported,
		boardForChip,
		isEspressifAppPort,
		ESPRESSIF_VID,
		type BoardMeta,
		type DeviceInfo,
		type FirmwareMeta
	} from '$lib/installer';
	import {
		CANONICAL_URL,
		SITE_TITLE,
		SITE_DESCRIPTION,
		SITE_KEYWORDS,
		SITE_NAME,
		SITE_LOCALE,
		OG_IMAGE,
		OG_IMAGE_WIDTH,
		OG_IMAGE_HEIGHT,
		OG_IMAGE_ALT,
		VIDEO,
		ARTIST,
		PARENT_PROJECT,
		REPO_URL,
		jsonLd
	} from '$lib/seo';

	const LD_JSON = `<script type="application/ld+json">${jsonLd()}</scr` + 'ipt>';

	type Stage = 'idle' | 'connecting' | 'ready' | 'flashing' | 'done' | 'error';

	let backdrop: HTMLCanvasElement;
	let galaxy: GalaxyHandle | null = null;

	let supported = true;
	let stage: Stage = 'idle';
	let error = '';
	let errorHint = '';
	let progress = 0;
	let log: string[] = [];

	let installer = new Installer();
	let device: DeviceInfo | null = null;
	let meta: FirmwareMeta | null = null;
	let board: BoardMeta | null = null;

	// Set once the board answers: what it actually is, against what is selected.
	let detected: BoardMeta | null = null;
	// True when the picked port is an ESP32-S3 running its application rather
	// than its ROM bootloader: flashable only after entering download mode.
	let appPort = false;
	// VID/PID of the port we last tried, so a failure can be explained in terms of
	// the board it actually was.
	let portIds: { vid: number; pid: number } | null = null;
	$: mismatch = detected !== null && board !== null && detected.id !== board.id;
	$: canInstall = stage === 'ready' && board !== null && !mismatch;

	onMount(async () => {
		supported = isSupported();
		galaxy = createGalaxy(backdrop);

		try {
			// A cached manifest or image here means flashing stale firmware, so
			// bypass the HTTP cache.
			const res = await fetch(`${base}/firmware/firmware.json`, { cache: 'no-store' });
			meta = await res.json();
			board = meta?.boards?.[0] ?? null;
		} catch {
			error = 'The firmware manifest did not load.';
		}
	});

	onDestroy(() => galaxy?.destroy());

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
		const noBootloader =
			/Failed to connect|Timed out waiting for packet|invalid head of packet|no sync|Cannot read|Wrong boot mode/i.test(
				message
			);

		// An Espressif device is an ESP32-S3 on its own USB, so none of the advice
		// below applies to it: there is no bridge, no DTR/RTS circuit and no cable
		// that will fix it. It just was not in download mode.
		if (noBootloader && portIds?.vid === ESPRESSIF_VID) {
			return (
				'The CoreS3 was not in download mode. Hold the reset button on the left ' +
				'side for 2–3 seconds until the green LED lights, let go, then connect ' +
				'again and pick "USB JTAG/serial debug unit". This is only needed while ' +
				'the board still has its factory firmware — once Lyrebird is installed it ' +
				'keeps the same USB identity across a reset and reflashes normally.'
			);
		}

		// Board never entered the ROM loader — auto-reset did not take. The screen
		// blanking and coming back several times during this is the reset sequence
		// working as intended, so say so: it reads like a fault and is not one.
		if (noBootloader) {
			return (
				'The board reset but started your firmware instead of its bootloader. ' +
				'(The screen going dark and back a few times during a connect is the reset ' +
				'sequence, not a fault.) In order, the things that cause this: a charge-only ' +
				'USB cable; a hub or a low-power port, which browns the Fire out under the ' +
				'flash write; and a serial monitor still holding the port. The M5Stack Core ' +
				'has no BOOT button, so it cannot be put into download mode by hand — if the ' +
				'reset never takes over this cable, use esptool from section 3, which drives ' +
				'the same lines from the OS with tighter timing. Open the esptool log below: ' +
				'"Wrong boot mode detected" means the reset is the problem, and "no sync reply" ' +
				'means the cable or the port is.'
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
		if (clean) log = [...log, clean].slice(-600);
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

		portIds = installer.portIds();
		appPort = isEspressifAppPort(portIds);
		if (appPort) {
			// Do not even try: the reset would drop this USB device and the browser
			// would lose the handle, which looks exactly like a board rebooting
			// forever. Say what to do instead.
			error = 'That port is the board running its own firmware, not its bootloader.';
			errorHint = '';
			stage = 'error';
			return;
		}

		stage = 'connecting';
		try {
			device = await installer.connect(addLog);
			// Trust the chip over the dropdown: an ESP32 image on an ESP32-S3
			// flashes without complaint and then never boots.
			detected = meta ? boardForChip(device.chip, meta.boards) : null;
			if (detected) {
				board = detected;
				addLog(`--- detected ${detected.name} (${detected.chipFamily})`);
			}
			stage = 'ready';
		} catch (e) {
			await installer.disconnect();
			device = null;
			// The handle may point at a USB device that no longer exists; make the
			// next attempt pick one again rather than retry against a ghost.
			installer.forgetPort();
			fail(e, 'The board did not answer. Unplug it, plug it in again, and retry.');
		}
	}

	async function install() {
		if (!board) return;
		error = '';
		errorHint = '';
		stage = 'flashing';
		progress = 0;

		try {
			const version = meta?.version ?? 'dev';
			const res = await fetch(
				`${base}/firmware/${board.image}?v=${encodeURIComponent(version)}`,
				{ cache: 'no-store' }
			);
			const image = new Uint8Array(await res.arrayBuffer());

			await installer.flash(board, image, (f) => (progress = f));

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
		detected = null;
		appPort = false;
		error = '';
		errorHint = '';
		log = [];
		progress = 0;
		stage = 'idle';
	}

	// --- Fallback path, for browsers and machines without working Web Serial ---

	$: manualCommand = board
		? `esptool.py --chip ${board.chip} write_flash 0x0 ${board.image}`
		: '';

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
	<link rel="canonical" href={CANONICAL_URL} />
	<meta property="og:type" content="website" />
	<meta property="og:site_name" content={SITE_NAME} />
	<meta property="og:title" content={SITE_TITLE} />
	<meta property="og:description" content={SITE_DESCRIPTION} />
	<meta property="og:url" content={CANONICAL_URL} />
	<meta property="og:locale" content={SITE_LOCALE} />
	<meta property="og:image" content={OG_IMAGE} />
	<meta property="og:image:width" content={String(OG_IMAGE_WIDTH)} />
	<meta property="og:image:height" content={String(OG_IMAGE_HEIGHT)} />
	<meta property="og:image:alt" content={OG_IMAGE_ALT} />
	<meta name="twitter:card" content="summary_large_image" />
	<meta name="twitter:title" content={SITE_TITLE} />
	<meta name="twitter:description" content={SITE_DESCRIPTION} />
	<meta name="twitter:image" content={OG_IMAGE} />
	<meta name="twitter:image:alt" content={OG_IMAGE_ALT} />
	<meta property="og:video" content={VIDEO.url} />
	<meta property="og:video:secure_url" content={VIDEO.url} />
	<meta property="og:video:type" content={VIDEO.type} />
	<meta property="og:video:width" content={String(VIDEO.width)} />
	<meta property="og:video:height" content={String(VIDEO.height)} />
	{@html LD_JSON}
</svelte:head>

<canvas class="backdrop" bind:this={backdrop} aria-hidden="true"></canvas>

<main>
	<header>
		<div class="titleline">
			<h1>Lyrebird</h1>
			<span class="badge">M5Stack Fire · CoreS3</span>
		</div>
		<p class="strap">
			<span>ESP32 / ESP32-S3</span><b>/</b>
			<span>2423 species · 12724 syllables</span><b>/</b>
			<span>22.05 kHz, synthesized on the board</span><b>/</b>
			<span>no SD card, no samples</span>
		</p>

		<p class="lede">
			A pocket dawn chorus. Two thousand four hundred songbirds, integrated in real time
			from the Mindlin–Laje model of the avian vocal organ — the same oscillator as
			<a href={PARENT_PROJECT.url}>{PARENT_PROJECT.name}</a>, ported to the board. No audio is
			stored: every note is computed from the two parameters real birds control, air-sac
			pressure and syringeal tension.
		</p>
		<p class="lede">
			It boots into <b>all birds</b> — twelve species at a time, two individuals of each,
			arriving as a Poisson process, and the twelve turn over every few minutes so the corpus
			comes past you rather than sitting still. Step off that with a button and you get one
			species, alone. Plug a Fire or a CoreS3 in and flash it from this page.
		</p>

		<!-- Both boards, running this firmware, with the sound they were making. It is the
		     one thing on the page that is not a claim: 28 s of hardware on a desk.
		     `preload="metadata"` so a visitor pays for the poster and not the 10 MB, and
		     no autoplay, because it has sound and nobody asked for it yet. -->
		<figure class="demo">
			<video
				controls
				playsinline
				preload="metadata"
				poster="{base}/lyrebird-boards-poster.jpg"
				width="1280"
				height="720"
			>
				<source src="{base}/lyrebird-boards.mp4" type="video/mp4" />
				<!-- No speech in the clip, so the track describes the sound instead of
				     transcribing it — see the note in the .vtt. -->
				<track kind="captions" src="{base}/lyrebird-boards.vtt" srclang="en" label="English" default />
				<p>
					Your browser cannot play this. <a href="{base}/lyrebird-boards.mp4">Download the
					clip</a> (MP4, 10 MB).
				</p>
			</video>
			<figcaption>
				A Fire and a CoreS3 on the all-birds slot, <b>with sound</b>. Both are drawing the
				same picture: a camera inside the corpus, following whoever is singing. The Fire has
				three real buttons; the CoreS3 has the same three drawn along the bottom as touch
				zones.
			</figcaption>
		</figure>
	</header>

	{#if !supported}
		<div class="notice warn">
			<strong>This browser cannot reach USB devices.</strong>
			<p>
				Web Serial is required. Recent Chrome, Edge and Opera have it; Safari and mobile
				browsers do not. Use the manual route below — it writes the same image on any machine.
			</p>
		</div>
	{:else}
		<section class="step" class:muted={stage !== 'idle' && stage !== 'connecting'}>
			<h2><span class="num">01</span> Connect</h2>

			{#if device}
				<dl class="deflist">
					<div><dt>Chip</dt><dd class="mono">{device.chip}</dd></div>
					<div><dt>MAC</dt><dd class="mono">{device.mac}</dd></div>
					<div><dt>USB</dt><dd class="mono">{device.port}</dd></div>
					<div><dt>Board</dt><dd>{detected?.name ?? 'not recognised'}</dd></div>
				</dl>
			{:else}
				<p>
					A data USB cable, not a charge-only one. The picker is filtered to USB-UART
					bridges and Espressif devices — the Fire's bridge is in the first group (a CP2104
						on older units, a CH9102 on a v2.7), and the
					CoreS3 appears as an Espressif device because it programs over the ESP32-S3's own
					USB rather than a bridge.
				</p>
				<div class="actions">
					<button class="primary" on:click={() => connect()} disabled={stage === 'connecting'}>
						{stage === 'connecting' ? 'Connecting…' : 'Connect device'}
					</button>
					<button on:click={() => connect(true)} disabled={stage === 'connecting'}>
						Show all ports
					</button>
				</div>
				<p class="note">
					<b>Fire:</b> just click Connect. The screen goes dark and comes back several times —
					that is the reset sequence putting the board into its bootloader, not a fault, and it
					can take half a minute. The Core has no BOOT button, so the reset has to happen over
					the cable.
				</p>
				<p class="note">
					<b>CoreS3, first time only:</b> put it into download mode before connecting. Hold the
					reset button on the left side for 2–3 seconds until the green LED lights, then let
					go. Click Connect and pick <span class="mono">USB JTAG/serial debug unit</span> — a
					new entry, not the one named after the board.
				</p>
				<p class="note">
					Why only the first time: the factory firmware puts the CoreS3 on the USB bus under
					its own identity, and resetting into the bootloader swaps it for another — the
					browser loses the port mid-connect. Lyrebird uses the chip's own USB-Serial-JTAG
					instead, which survives a reset, so once it is installed the board reflashes from
					here like any other.
				</p>
			{/if}
		</section>

		<section class="step">
			<h2><span class="num">02</span> Board</h2>
			<p>
				The Fire is an ESP32 and the CoreS3 an ESP32-S3 — different processors, so there is
				an image for each and no one binary that runs on both. Connecting above read the chip
				off the board and picked for you. Change it only if you know better than the chip did.
			</p>
			<div class="actions">
				{#each meta?.boards ?? [] as b (b.id)}
					<button class:on={board?.id === b.id} on:click={() => (board = b)}>
						{b.name}
					</button>
				{/each}
			</div>
			{#if mismatch && detected}
				<div class="notice warn">
					<strong>That is not the board you have.</strong>
					<p>
						The chip answered as {detected.chipFamily}, so this is a {detected.name}. Writing
						the other image would flash without complaint and then never boot, so Install
						stays disabled until the selection matches.
					</p>
				</div>
			{/if}
		</section>

		<section class="step" class:muted={!device}>
			<h2><span class="num">03</span> Install</h2>

			{#if stage === 'flashing'}
				<div class="bar"><span style="width: {Math.round(progress * 100)}%"></span></div>
				<p class="note mono">writing firmware — {Math.round(progress * 100)} %</p>
			{:else if stage === 'done'}
				<div class="notice ok">
					<strong>Installed.</strong>
					<p>
						The board restarts on its own and boots into the all-birds chorus. If the screen
						stays dark, press the red power button once.
					</p>
				</div>
				<div class="actions"><button on:click={startOver}>Flash another board</button></div>
			{:else}
				<div class="actions">
					<button class="primary" on:click={install} disabled={!canInstall}>
						Install Lyrebird
					</button>
				</div>
				<p class="note">
					Writes bootloader, partition table and firmware as one image. About a minute.
					{#if meta}<span class="mono">build {meta.version}</span>{/if}
				</p>
			{/if}

			{#if error && appPort}
				<div class="notice warn">
					<strong>That port is the board, not its bootloader.</strong>
					<p>
						An ESP32-S3 with native USB is two different USB devices: the firmware it is
						running presents one, and the ROM bootloader presents another. Only the second
						can be flashed, and resetting into it <i>replaces</i> the USB device — so the
						port the browser just granted would stop existing halfway through, which is
						what makes the board look like it is restarting forever.
					</p>
					<p>
						Hold the reset button on the left side for 2–3 seconds until the green LED
						lights, release, then Connect again and choose
						<span class="mono">USB JTAG/serial debug unit</span>.
					</p>
					<div class="actions">
						<button class="primary" on:click={() => connect(true)}>Pick the port again</button>
					</div>
				</div>
			{:else if error}
				<div class="notice warn">
					<strong>The install did not finish.</strong>
					<p class="mono errline">{error}</p>
					{#if errorHint}<p class="errhint">{errorHint}</p>{/if}
					{#if log.length}
						<details>
							<summary>esptool log — {log.length} lines</summary>
							<pre>{log.join('\n')}</pre>
						</details>
					{/if}
					<div class="actions">
						<button class="primary" on:click={() => connect()}>Pick the port and retry</button>
						<button on:click={startOver}>Start over</button>
					</div>
				</div>
			{/if}
		</section>
	{/if}

	<section class="step" id="manual">
		<h2><span class="num">04</span> Without Web Serial</h2>
		<p>
			Safari, mobile, or any browser where the picker stays empty. The same image, written by
			<a href="https://docs.espressif.com/projects/esptool/en/latest/esp32/">esptool</a>
			(<code>pip install esptool</code>).
		</p>

		<div class="actions">
			<a
				class="button-like"
				href="{base}/firmware/{board?.image ?? 'lyrebird-fire.bin'}"
				download
			>
				Download {board?.name ?? 'firmware'}
			</a>
			<button on:click={copyCommand} disabled={!manualCommand}>
				{copied ? 'Copied' : 'Copy command'}
			</button>
		</div>

		<pre class="command"><code>{manualCommand}</code></pre>

		<p class="note">
			Run it beside the downloaded file. Add <code>--port /dev/ttyUSB0</code> (or
			<code>COM3</code>) if esptool picks the wrong one. On a permission error, run
			<code>sudo usermod -aG dialout $USER</code>, then log out and log in again.
		</p>
	</section>

	<section class="step">
		<h2><span class="num">05</span> Controls</h2>
		<p>
			Three buttons under the screen. A and C step one dial of 2424 positions; B changes how
			the current position sings. A hold does something else than a press.
		</p>
		<p class="note">
			The CoreS3 has no physical buttons — the same three live on the touch strip in the
			bezel below the screen, left to right.
		</p>

		<dl class="deflist">
			<div>
				<dt>A · C short</dt>
				<dd>
					Previous / next position on the dial. Position 1 is <b>all birds</b> — twelve
					species at a time, rolling. Positions 2–2424 are one species each, and it wraps.
					One press is one position and there is no search, so the dial is for wandering.
				</dd>
			</div>
			<div>
				<dt>B short</dt>
				<dd>
					Chorus ↔ solo, on a single-species position: four individuals answering each other,
					or one bird singing its songs back to back. All birds is a chorus by definition, so
					B does nothing there.
				</dd>
			</div>
			<div><dt>A · C hold</dt><dd>Volume down / up, in 2 % steps.</dd></div>
			<div>
				<dt>B hold</dt>
				<dd>Pause and resume. Paused, the Fire's DAC goes high-Z — no idle hiss.</dd>
			</div>
		</dl>

		<p class="note">
			<b>The screen puts a camera inside the corpus.</b> Nothing is drawn for a bird until
			it sings, so the band is black until something happens. When a bird starts a song, a
			cross of dashed rules is struck through it and a thread of stretched dots grows out
			of it — fat where the note is loud, pinched where it is quiet, with a haloed bead at
			the note being sung right now. The camera frames whoever is singing: close in on
			one, easing back to hold two together, and easing back again as a long song
			outgrows the frame.
		</p>

		<p class="note">
			So the screen answers a different question than a spectrogram would: not what pitch
			is sounding, but which of the 2423 birds it is, where that bird sits among the rest,
			and what its song is doing.
		</p>
	</section>

	<footer>
		<div class="links">
			<a href={REPO_URL}>Source</a>
			<a href={PARENT_PROJECT.url}>{PARENT_PROJECT.name}</a>
			<a href={ARTIST.url}>{ARTIST.name}</a>
		</div>
		<p class="note mono">variable.gallery</p>
	</footer>
</main>

<style>
	/**
	 * The page's own arrangement. Everything reusable — tokens, controls, type — is in
	 * app.css, matching the parent project's split.
	 *
	 * The rule of the ground: hairlines, not boxes. No panel here has a background, a
	 * shadow or a blur; a section is separated from the next by a 1 px rule, and the only
	 * saturated colour is `--signal` on the one live thing per state.
	 */

	/**
	 * The corpus, behind everything (see $lib/galaxy). Fixed rather than scrolling,
	 * because it is the ground the page stands on and not an illustration of any one
	 * section — the parent project's galaxy does not scroll away either.
	 *
	 * The mask is the only reason this is readable: it thins the cloud through the
	 * middle third, which is exactly where the text column is, and leaves it whole in
	 * the margins. Everything else about keeping type legible is done in the renderer's
	 * ink budget rather than here.
	 */
	.backdrop {
		position: fixed;
		inset: 0;
		z-index: 0;
		width: 100%;
		height: 100%;
		pointer-events: none;
		-webkit-mask-image: radial-gradient(
			ellipse 46% 44% at 50% 45%,
			rgba(0, 0, 0, 0.68) 0%,
			rgba(0, 0, 0, 0.85) 60%,
			#000 100%
		);
		mask-image: radial-gradient(
			ellipse 46% 44% at 50% 45%,
			rgba(0, 0, 0, 0.68) 0%,
			rgba(0, 0, 0, 0.85) 60%,
			#000 100%
		);
	}

	/* A phone has no margins for the cloud to live in, so every mark it draws lands
	   under a line of type. Same picture, less ink. */
	@media (max-width: 48rem) {
		.backdrop {
			opacity: 0.55;
		}
	}

	main {
		position: relative;
		z-index: 1;
		max-width: 46rem;
		margin: 0 auto;
		padding: clamp(2.5rem, 7vw, 5rem) 1.5rem 4rem;
	}

	/* ------------------------------------------------------------------ header -- */

	header {
		padding-bottom: 1.6rem;
		margin-bottom: 2.4rem;
		border-bottom: 1px solid var(--ink);
	}

	.titleline {
		display: flex;
		align-items: baseline;
		flex-wrap: wrap;
		gap: 0.8rem;
	}

	h1 {
		font-size: clamp(1.5rem, 4vw, 2.1rem);
		letter-spacing: 0.01em;
	}

	.badge {
		padding: 0.05rem 0.4rem;
		border: 1px solid var(--rule-strong);
		border-radius: var(--radius);
		font-family: var(--font-mono);
		font-size: 0.62rem;
		letter-spacing: 0.08em;
		text-transform: uppercase;
		color: var(--ink-dim);
	}

	/* The figures line. Monospace because each item is a measurement. */
	.strap {
		display: flex;
		flex-wrap: wrap;
		gap: 0.1rem 0.5rem;
		margin: 0.7rem 0 1.4rem;
		font-family: var(--font-mono);
		font-size: 0.66rem;
		letter-spacing: 0.04em;
		color: var(--ink-faint);
	}

	.strap b {
		font-weight: 400;
		color: var(--rule-strong);
	}

	.lede {
		max-width: 40rem;
		margin: 0 0 0.8rem;
		color: var(--ink-dim);
	}

	.lede b {
		color: var(--ink);
		font-weight: 600;
	}

	/* The clip of the two boards. Full width of the column and no wider: it is a 16:9
	   frame of two small screens, and shrinking it further makes the band unreadable. */
	.demo {
		max-width: 40rem;
		margin: 1.2rem 0 0;
	}

	.demo video {
		display: block;
		width: 100%;
		height: auto;
		border: 1px solid var(--rule);
		border-radius: var(--radius);
		background: #000;
	}

	.demo figcaption {
		max-width: 34rem;
		margin: 0.5rem 0 0;
		font-size: 0.8rem;
		line-height: 1.5;
		color: var(--ink-faint);
	}

	.demo figcaption b {
		color: var(--ink-dim);
		font-weight: 600;
	}

	/* ------------------------------------------------------------------- steps -- */

	.step {
		padding-bottom: 2.2rem;
		transition: opacity 220ms ease;
	}

	/* A step that is not this step's turn. Not hidden — the reader should still be able
	   to read ahead and see what is coming. */
	.step.muted {
		opacity: 0.4;
	}

	h2 {
		display: flex;
		align-items: baseline;
		gap: 0.7rem;
		margin: 0 0 0.9rem;
		padding-bottom: 0.45rem;
		border-bottom: 1px solid var(--rule);
		font-size: 0.95rem;
		letter-spacing: 0.02em;
	}

	.num {
		flex: none;
		font-family: var(--font-mono);
		font-size: 0.7rem;
		color: var(--ink-faint);
	}

	.step p {
		max-width: 40rem;
		margin: 0 0 0.9rem;
		color: var(--ink-dim);
	}

	.note {
		font-size: 0.78rem;
		color: var(--ink-faint);
	}

	.actions {
		display: flex;
		flex-wrap: wrap;
		gap: 0.5rem;
		align-items: center;
		margin-bottom: 0.9rem;
	}

	/* --------------------------------------------------------------- deflists -- */

	/* A term and what it is, ruled rather than boxed — the parent's `.deflist`. */
	.deflist {
		margin: 0 0 1rem;
		max-width: 40rem;
	}

	.deflist > div {
		display: grid;
		grid-template-columns: 7.5rem minmax(0, 1fr);
		gap: 0.2rem 1rem;
		padding: 0.55rem 0;
		border-top: 1px solid var(--rule);
	}

	.deflist > div:last-child {
		border-bottom: 1px solid var(--rule);
	}

	@media (max-width: 34rem) {
		.deflist > div {
			grid-template-columns: 1fr;
		}
	}

	.deflist dt {
		font-family: var(--font-mono);
		font-size: 0.72rem;
		color: var(--ink);
	}

	.deflist dd {
		margin: 0;
		font-size: 0.82rem;
		line-height: 1.6;
		color: var(--ink-dim);
		overflow-wrap: anywhere;
	}

	.deflist dd b {
		color: var(--ink);
		font-weight: 600;
	}

	/* -------------------------------------------------------------- progress -- */

	/* 3 px, because the bar is a readout and not a feature. */
	.bar {
		height: 3px;
		background: var(--rule);
		border-radius: 2px;
		overflow: hidden;
	}

	.bar span {
		display: block;
		height: 100%;
		background: var(--signal);
		transition: width 200ms ease;
	}

	/* --------------------------------------------------------------- notices -- */

	/* The one place a colour other than graphite appears on a block: a 2 px edge, so a
	   failed write is findable by scanning the left margin. */
	.notice {
		margin: 0 0 1rem;
		padding: 0.7rem 0 0.7rem 0.9rem;
		border-left: 2px solid var(--rule-strong);
	}

	.notice.warn {
		border-left-color: var(--warn);
	}

	.notice.ok {
		border-left-color: var(--ok);
	}

	.notice strong {
		display: block;
		font-weight: 600;
		color: var(--ink);
	}

	.notice p {
		margin: 0.25rem 0 0;
		font-size: 0.84rem;
	}

	.notice .actions {
		margin: 0.8rem 0 0;
	}

	.errline {
		color: var(--warn);
		user-select: text;
	}

	.errhint {
		margin-top: 0.6rem;
		padding-top: 0.6rem;
		border-top: 1px solid var(--rule);
		user-select: text;
	}

	/* ------------------------------------------------------------------- logs -- */

	summary {
		margin: 0.7rem 0 0;
		font-size: 0.74rem;
		color: var(--ink-faint);
		cursor: pointer;
	}

	summary:hover {
		color: var(--signal);
	}

	pre {
		max-height: 14rem;
		margin: 0.5rem 0 0;
		padding: 0.7rem 0.8rem;
		overflow: auto;
		border: 1px solid var(--rule);
		border-radius: var(--radius);
		background: var(--paper-raised);
		font-family: var(--font-mono);
		font-size: 0.7rem;
		line-height: 1.65;
		color: var(--ink-dim);
		user-select: text;
	}

	.command {
		max-height: none;
		color: var(--ink);
		white-space: pre;
	}

	.command code {
		padding: 0;
		border: 0;
		background: none;
		font-size: inherit;
	}

	/* ----------------------------------------------------------------- footer -- */

	footer {
		display: flex;
		flex-wrap: wrap;
		gap: 0.6rem 1.5rem;
		align-items: baseline;
		justify-content: space-between;
		margin-top: 1.5rem;
		padding-top: 1rem;
		border-top: 1px solid var(--ink);
	}

	.links {
		display: flex;
		flex-wrap: wrap;
		gap: 1.2rem;
		font-size: 0.8rem;
	}

	footer p {
		margin: 0;
		font-size: 0.7rem;
		color: var(--ink-faint);
	}
</style>
