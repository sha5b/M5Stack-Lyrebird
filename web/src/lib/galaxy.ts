/**
 * The page's backdrop: the corpus as a point cloud, in the galaxy's own terms.
 *
 * This is the flasher's answer to CYD-Physarum's physarum backdrop — there, the
 * page runs the same simulation the panel runs. Here the device draws a
 * spectrogram, which is a picture of one moment, so a page-sized version of it
 * would be a strip of noise. What the parent project draws instead is the whole
 * inventory at once (`app/src/lib/galaxy/`), and that is what a page can hold:
 * every syllable the firmware knows, as one mark. All 12724 of them.
 *
 * Borrowed from the parent's galaxy, deliberately:
 *
 * - **Position carries species, not colour.** Twelve hues cannot be told apart on
 *   a scatter and 2423 certainly cannot, so every species gets an island of its
 *   own and hue is not spent on identity at all (`galaxy/palette.ts` makes this
 *   argument at length).
 * - **rest -> ink is response.** A pale mark darkens as its island answers, and
 *   depth is the resting half of that same contrast. It carries more range than a
 *   walk through any single hue.
 * - **One accent, one meaning.** `--signal` marks the island singing right now and
 *   nothing else — the same thing it means on a button.
 * - **Hairlines, not boxes.** The threads are 1 px at low alpha, and there is no
 *   frame, no panel and no blur anywhere in the picture.
 *
 * Where the marks go is not decided here: tools/generate_galaxy.py lays the cloud
 * out once and writes it to `corpus.ts` for this renderer and to
 * `include/galaxy_data.h` for the band on the device's screen, so the page and the
 * board cannot disagree about where a syllable is. That generator's header comment
 * is the one place the layout is explained.
 *
 * What it is not: a learned embedding. The parent's galaxy is a real projection of
 * fitted vectors; this arranges four quantised features.
 */

import { cloud, SPECIES } from './corpus';

/**
 * Kept in step with app.css by hand, for the reason `galaxy/palette.ts` gives:
 * the canvas needs numbers and a custom property is a string resolved at paint
 * time. `INK` is `--ink`, `ACCENT` is `--signal`.
 */
const INK = '16, 22, 31';
const ACCENT = '91, 63, 214';

/**
 * Ink budget. This canvas sits behind body text, so the whole picture is drawn
 * quieter than any type on top of it — a backdrop that competes with prose is a
 * failure however good it looks. REST is a mark in an island that is not
 * answering, at full depth; LIT is the same mark at full response.
 */
const REST = 0.21;
const LIT = 0.7;
const THREAD = 0.3;

/**
 * How many islands answer at once, how long each takes, and the dark between.
 * More than one because the thing this page flashes onto a board is a *chorus* —
 * a single mark lighting and fading in turn would be the solo mode.
 */
const VOICES = 3;
const SING_MS = 2600;
const GAP_MS = 900;

const TURN_X = 197_000; // ms per revolution — mutually prime-ish periods, so the
const TURN_Y = 131_000; // cloud never returns to the same pose twice in a sitting

/**
 * Alpha buckets. 12724 marks a frame is nothing to fill and everything to
 * *style*: a fillStyle string per mark means 12724 colour parses per frame, which
 * is the whole budget. Quantising alpha into a few buckets makes it a dozen
 * fillStyle assignments and a dozen paths instead.
 */
const BUCKETS = 10;

export interface GalaxyHandle {
	destroy(): void;
}

/** Deterministic hash -> [0, 1). Islands must sit still across reloads. */
function hash01(n: number, salt: number): number {
	let h = (n * 0x9e3779b1 + salt * 0x85ebca6b) >>> 0;
	h ^= h >>> 15;
	h = (h * 0x2545f491) >>> 0;
	h ^= h >>> 13;
	return (h >>> 0) / 4294967296;
}

export function createGalaxy(canvas: HTMLCanvasElement): GalaxyHandle {
	const ctx = canvas.getContext('2d');
	if (!ctx) return { destroy() {} };

	const c = cloud();
	const n = c.count;
	const px = c.x;
	const py = c.y;
	const pz = c.z;
	// A buzz is a fainter mark than a whistle: the timbre classes carry how much of
	// the sound is actually pitched.
	const weight = c.shade;
	// Marks arrive in species order, so an island is a contiguous run and the accent
	// pass needs two integers rather than a list per species.
	const islandStart = c.start;

	const still = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

	// ---- screen space ------------------------------------------------------
	const sx = new Float32Array(n);
	const sy = new Float32Array(n);
	const alpha = new Float32Array(n);
	// Per-bucket scratch, refilled each frame rather than allocated.
	const bx: Float32Array[] = Array.from({ length: BUCKETS }, () => new Float32Array(n));
	const by: Float32Array[] = Array.from({ length: BUCKETS }, () => new Float32Array(n));
	const bn = new Uint32Array(BUCKETS);

	let w = 0;
	let h = 0;
	let scale = 0;
	let cx = 0;
	let cy = 0;

	function resize() {
		const dpr = Math.min(2, window.devicePixelRatio || 1);
		w = canvas.clientWidth;
		h = canvas.clientHeight;
		canvas.width = Math.max(1, Math.round(w * dpr));
		canvas.height = Math.max(1, Math.round(h * dpr));
		ctx!.setTransform(dpr, 0, 0, dpr, 0, 0);
		// Sized off the diagonal, which keeps the marks-per-area roughly constant
		// across window shapes. Off the short side, a phone concentrates all 12724
		// into a third of the area and the cloud turns into a stain behind the
		// lede; off the long side, a wide window spreads them to 11 px apart and
		// the cloud turns into paper grain. The diagonal is the one measure that
		// gives a body with an edge either way.
		scale = Math.hypot(w, h) * 0.3;
		cx = w / 2;
		cy = h * 0.45;
	}

	function project(t: number) {
		// Yaw then pitch. A two-axis drift reads as a body turning; one axis reads
		// as a disc spinning, which is a screensaver.
		const ax = still ? 0.42 : (t / TURN_X) * Math.PI * 2;
		const ay = still ? 0.9 : (t / TURN_Y) * Math.PI * 2;
		const cax = Math.cos(ax);
		const sax = Math.sin(ax);
		const cay = Math.cos(ay);
		const say = Math.sin(ay);

		bn.fill(0);

		for (let i = 0; i < n; i++) {
			const x1 = px[i] * cay + pz[i] * say;
			const z1 = -px[i] * say + pz[i] * cay;
			const y2 = py[i] * cax - z1 * sax;
			const z2 = py[i] * sax + z1 * cax;

			const persp = 2.9 / (2.9 + z2); // gentle: enough to tell near from far
			const X = cx + x1 * scale * persp;
			const Y = cy - y2 * scale * persp;
			sx[i] = X;
			sy[i] = Y;

			// depth 0 far .. 1 near
			const d = z2 < -1 ? 0 : z2 > 1 ? 1 : (z2 + 1) / 2;
			const a = REST * weight[i] * (0.18 + 0.82 * d);
			alpha[i] = a;

			// Off-canvas marks still need their alpha for the accent pass, but not a
			// place in a path.
			if (X < -2 || Y < -2 || X > w + 2 || Y > h + 2) continue;

			const b = Math.min(BUCKETS - 1, Math.floor((a / REST) * BUCKETS));
			const k = bn[b]++;
			bx[b][k] = X;
			by[b][k] = Y;
		}
	}

	/**
	 * Which island voice `v` is answering, and how much. Islands are drawn from the
	 * whole corpus rather than walked in order, so an answer lands somewhere new
	 * across the picture each time instead of creeping along the sphere. The
	 * voices are offset by an odd fraction of a cycle so they never fall into step.
	 */
	function singing(t: number, v: number): { island: number; amount: number } {
		const cycle = SING_MS + GAP_MS;
		const tv = t + v * cycle * 0.41;
		const step = Math.floor(tv / cycle);
		const phase = (tv % cycle) / SING_MS;
		const island = Math.floor(hash01(step * 31 + v, 7) * SPECIES);
		if (phase >= 1) return { island, amount: 0 };
		// in fast, out slow: a syllable's own envelope shape
		const amount = phase < 0.14 ? phase / 0.14 : 1 - (phase - 0.14) / 0.86;
		return { island, amount: Math.max(0, amount) };
	}

	function draw(t: number) {
		ctx!.clearRect(0, 0, w, h);
		project(t);

		// The cloud at rest, a bucket at a time.
		for (let b = 1; b < BUCKETS; b++) {
			const count = bn[b];
			if (!count) continue;
			const a = (REST * (b + 0.5)) / BUCKETS;
			// Near marks are a touch bigger as well as darker; the two together are
			// what make a flat scatter read as a volume.
			const r = b > BUCKETS * 0.78 ? 2 : b > BUCKETS * 0.45 ? 1.5 : 1.2;
			const half = r / 2;
			ctx!.fillStyle = `rgba(${INK}, ${a.toFixed(3)})`;
			ctx!.beginPath();
			const xs = bx[b];
			const ys = by[b];
			for (let i = 0; i < count; i++) ctx!.rect(xs[i] - half, ys[i] - half, r, r);
			ctx!.fill();
		}

		if (still) return;

		// The islands that are answering: their own marks in the accent, and a
		// thread through each, because a species is a thing with parts and the
		// thread is what says so.
		for (let v = 0; v < VOICES; v++) {
			const lit = singing(t, v);
			const k = lit.amount;
			if (k <= 0) continue;

			const from = islandStart[lit.island];
			const to = islandStart[lit.island + 1];
			if (to <= from) continue;

			if (to - from > 1) {
				// A path through the island in pitch order — the syllables of a song
				// climb and fall, so the thread is a contour rather than a web.
				const order: number[] = [];
				for (let i = from; i < to; i++) order.push(i);
				order.sort((a, b) => sy[a] - sy[b]);
				ctx!.strokeStyle = `rgba(${ACCENT}, ${(THREAD * k).toFixed(3)})`;
				ctx!.lineWidth = 1;
				ctx!.beginPath();
				ctx!.moveTo(sx[order[0]], sy[order[0]]);
				for (let i = 1; i < order.length; i++) ctx!.lineTo(sx[order[i]], sy[order[i]]);
				ctx!.stroke();
			}

			ctx!.fillStyle = `rgba(${ACCENT}, ${(LIT * k).toFixed(3)})`;
			ctx!.beginPath();
			const r = 1.8 + 1.6 * k;
			for (let i = from; i < to; i++) ctx!.rect(sx[i] - r / 2, sy[i] - r / 2, r, r);
			ctx!.fill();
		}
	}

	let raf = 0;
	let running = true;
	let t0 = 0;

	function loop(now: number) {
		if (!running) return;
		if (!t0) t0 = now;
		draw(now - t0);
		raf = requestAnimationFrame(loop);
	}

	const onResize = () => {
		resize();
		if (still) draw(0);
	};

	// A backdrop has no business burning a frame budget in a background tab.
	const onVisibility = () => {
		if (still) return;
		if (document.hidden) {
			running = false;
			cancelAnimationFrame(raf);
		} else if (!running) {
			running = true;
			raf = requestAnimationFrame(loop);
		}
	};

	resize();
	if (still) draw(0);
	else raf = requestAnimationFrame(loop);
	window.addEventListener('resize', onResize);
	document.addEventListener('visibilitychange', onVisibility);

	return {
		destroy() {
			running = false;
			cancelAnimationFrame(raf);
			window.removeEventListener('resize', onResize);
			document.removeEventListener('visibilitychange', onVisibility);
		}
	};
}
