import type { ClassicReset, ESPLoader, Transport } from 'esptool-js';

// What `LoaderOptions.resetConstructors.classicReset` is declared to return. See
// the note at the call site: the runtime only ever calls `.reset()` on it.
type ClassicResetLike = ClassicReset;

/**
 * Drives the whole connect -> flash cycle over Web Serial.
 *
 * The firmware is one merged 0x0-based image (bootloader + partitions + app,
 * produced by scripts/build-firmware.sh), written in a single part. There is
 * no config sector: the device needs no install-time settings.
 */

// USB-UART bridges. The M5Stack Fire's CP2104 shares the CP2102's VID:PID.
// Filtering by these keeps the browser's port picker to real devices instead
// of every ttyS*/COM* and Bluetooth port.
const USB_FILTERS = [
	{ usbVendorId: 0x10c4, usbProductId: 0xea60 }, // Silicon Labs CP2102/CP2104 (M5Stack Fire)
	{ usbVendorId: 0x10c4, usbProductId: 0xea70 }, // Silicon Labs CP2105
	{ usbVendorId: 0x1a86, usbProductId: 0x7523 }, // WCH CH340
	{ usbVendorId: 0x1a86, usbProductId: 0x55d3 }, // WCH CH343
	{ usbVendorId: 0x1a86, usbProductId: 0x55d4 }, // WCH CH9102
	{ usbVendorId: 0x0403, usbProductId: 0x6001 }, // FTDI FT232R
	// Espressif, vendor-wide: the CoreS3 programs over the ESP32-S3's own USB
	// rather than a bridge chip, so it appears as an Espressif CDC/JTAG device.
	// esptool-js detects PID 0x1001 and switches to its USB-JTAG reset by itself.
	{ usbVendorId: 0x303a }
];

// Sync always happens at 115200 (esptool-js `romBaudrate`); these are the rates
// the loader switches to *after* sync. Some bridges are unreliable above ~460800.
const FAST_BAUD = 460800;
const SLOW_BAUD = 115200;

/**
 * Bounds one whole esptool connect, not one reset.
 *
 * It has to sit above esptool-js's own budget, and that budget is larger than it
 * looks: `ESPLoader.connect()` makes 7 attempts, each a reset sequence followed
 * by 5 sync tries. Below that ceiling we abort the loader mid-sequence and start
 * it again, which resets the board another seven times without ever letting it
 * finish — the board appears to restart forever and the reported error is
 * whatever our timeout says rather than what esptool found.
 */
const CONNECT_TIMEOUT_MS = 60000;

/**
 * Reset sequences for `CustomReset`: D = DTR (1 pulls IO0 low), R = RTS (1 pulls
 * EN low), W = wait in ms.
 *
 * esptool-js's built-in ClassicReset is `D0|R1|W100|D1|R0|W50|D0`, and on an
 * M5Stack Core that is marginal in two places. EN carries a debounce capacitor
 * for the side reset button, so 100 ms is not always long enough for the line to
 * reach a logic low — the chip never really enters reset. And releasing EN in the
 * statement straight after pulling IO0 low leaves no settling time, so the chip
 * can latch IO0 before it has actually gone low and boot the application instead
 * of the ROM loader. Over Web Serial each signal change is a separate async
 * round trip to the browser's serial stack, which widens both windows further.
 *
 * So: hold EN low longer, then insert an explicit wait between IO0 going low and
 * EN being released. esptool.py papers over the same board with its
 * `ESPTOOL_RESET_DELAY` escape hatch.
 *
 * Two lengths, because esptool alternates the strategies it is given across its
 * seven attempts — a board that needs the slow one still gets it.
 */
const RESET_SEQUENCE_SHORT = 'D0|R1|W200|D1|W40|R0|W450|D0';
const RESET_SEQUENCE_LONG = 'D0|R1|W500|D1|W80|R0|W900|D0';

function withTimeout<T>(work: Promise<T>, ms: number, label: string): Promise<T> {
	let timer: ReturnType<typeof setTimeout>;
	const expiry = new Promise<never>((_, reject) => {
		timer = setTimeout(() => reject(new Error(`${label} timed out after ${ms / 1000}s`)), ms);
	});
	return Promise.race([work, expiry]).finally(() => clearTimeout(timer)) as Promise<T>;
}

/** One flashable board. The Fire and the CoreS3 are different architectures, so
 * there is an image per board and no single binary that runs on both. */
export interface BoardMeta {
	id: string;
	name: string;
	chip: string; // esptool --chip argument
	chipFamily: string; // as esptool reports it: "ESP32", "ESP32-S3"
	image: string;
	imageOffset: number;
}

export interface FirmwareMeta {
	version: string;
	boards: BoardMeta[];
}

/**
 * Which board an esptool chip description belongs to.
 *
 * `device.chip` is a human string like "ESP32-D0WD-V3 (revision v3.1)" or
 * "ESP32-S3 (QFN56) (revision v0.2)", so this matches on the family rather than
 * parsing it. Writing an ESP32 image to an S3 is not fatal but it produces a
 * board that flashes happily and then never boots, which is a bad afternoon.
 */
export function boardForChip(chip: string, boards: BoardMeta[]): BoardMeta | null {
	const upper = chip.toUpperCase();
	// longest family name first, so "ESP32-S3" is not matched by "ESP32"
	const byLength = [...boards].sort((a, b) => b.chipFamily.length - a.chipFamily.length);
	return byLength.find((b) => upper.includes(b.chipFamily.toUpperCase())) ?? null;
}

export interface DeviceInfo {
	chip: string;
	mac: string;
	port: string;
}

export function isSupported(): boolean {
	return typeof navigator !== 'undefined' && 'serial' in navigator;
}

function sleep(ms: number): Promise<void> {
	return new Promise((resolve) => setTimeout(resolve, ms));
}

export class Installer {
	private port: SerialPort | null = null;
	private transport: Transport | null = null;
	private loader: ESPLoader | null = null;

	/** Open the browser port picker, filtered to known USB-UART bridges. */
	async requestPort(showAll = false): Promise<void> {
		const options = showAll ? {} : { filters: USB_FILTERS };
		this.port = await navigator.serial.requestPort(options);
	}

	hasPort(): boolean {
		return this.port !== null;
	}

	/**
	 * Enter the ROM bootloader and identify the chip.
	 *
	 * The board visibly resets several times while this runs — that is the reset
	 * sequence, not a fault.
	 *
	 * There is deliberately only one connect here. The previous version ran the
	 * whole thing twice, once per baud rate, which cannot help and does harm:
	 * sync happens at `romBaudrate` (115200) whatever `baudrate` is set to, so a
	 * board that will not sync at one rate will not sync at the other, and the
	 * second run just resets it another seven times. The fallback below only
	 * fires when the board *did* answer and the fast rate was what failed.
	 */
	async connect(onLog: (line: string) => void): Promise<DeviceInfo> {
		if (!this.port) throw new Error('No port selected');

		try {
			onLog(`--- connecting (sync at ${SLOW_BAUD}, then ${FAST_BAUD})`);
			return await withTimeout(this.attemptConnect(FAST_BAUD, onLog), CONNECT_TIMEOUT_MS, 'Connect');
		} catch (e) {
			// `chip` is only set once sync succeeded and the magic value was read.
			const answered = this.loader?.chip != null;
			onLog(`--- failed: ${e instanceof Error ? e.message : String(e)}`);
			await this.teardown();

			if (!answered) {
				// The board never reached the ROM loader. Resetting it again will
				// not change that, so stop rather than thrash it.
				throw e instanceof Error ? e : new Error(String(e));
			}

			onLog(`--- the board answered but ${FAST_BAUD} did not hold; retrying at ${SLOW_BAUD}`);
			return await withTimeout(this.attemptConnect(SLOW_BAUD, onLog), CONNECT_TIMEOUT_MS, 'Connect');
		}
	}

	private async attemptConnect(
		baudrate: number,
		onLog: (line: string) => void
	): Promise<DeviceInfo> {
		// Loaded on demand: esptool-js ships extensionless ESM imports that
		// Node cannot resolve during prerendering, and it is only ever needed
		// in the browser once the user has actually picked a device.
		const { ESPLoader, Transport, CustomReset } = await import('esptool-js');

		this.transport = new Transport(this.port!, false);
		this.loader = new ESPLoader({
			transport: this.transport,
			baudrate,
			// The reason a connect failed is otherwise thrown away: `connect()`
			// raises a flat "Failed to connect with the device" no matter what,
			// while the useful finding — "Wrong boot mode detected (0x13)" versus
			// "Download mode detected, but getting no sync reply" — goes only to
			// `debug()`. With this on it lands in the log the page already shows,
			// which is the difference between diagnosing this and guessing at it.
			debugLogging: true,
			resetConstructors: {
				// esptool hands this its own delay (50 ms, then 550 ms) and expects
				// a ClassicReset back. We substitute the two sequences above, which
				// is why the cast is here: upstream types the factory by the class
				// it happens to return rather than by the ResetStrategy interface
				// it actually uses, and CustomReset implements that interface.
				classicReset: (transport, resetDelay) =>
					new CustomReset(
						transport,
						resetDelay > 100 ? RESET_SEQUENCE_LONG : RESET_SEQUENCE_SHORT
					) as unknown as ClassicResetLike
			},
			terminal: {
				clean() {},
				writeLine: onLog,
				write: onLog
			}
		});

		const chip = await this.loader.main();
		const mac = await this.loader.chip.readMac(this.loader);
		const info = this.port!.getInfo();
		const vid = info.usbVendorId?.toString(16).padStart(4, '0') ?? '????';
		const pid = info.usbProductId?.toString(16).padStart(4, '0') ?? '????';

		return { chip, mac, port: `${vid}:${pid}` };
	}

	/**
	 * Write the firmware image in one pass.
	 * @param onProgress fraction 0..1
	 */
	async flash(
		board: BoardMeta,
		image: Uint8Array,
		onProgress: (fraction: number) => void
	): Promise<void> {
		if (!this.loader) throw new Error('Not connected');

		await this.loader.writeFlash({
			fileArray: [{ data: image, address: board.imageOffset }],
			flashMode: 'keep',
			flashFreq: 'keep',
			flashSize: 'keep',
			eraseAll: false,
			compress: true,
			reportProgress: (_fileIndex, written) => {
				onProgress(Math.min(1, written / image.length));
			}
		});
	}

	/**
	 * Release the board so it runs whatever is in flash.
	 *
	 * Entering the ROM loader leaves EN and GPIO0 asserted through RTS/DTR. If
	 * the port is simply closed after a failure the board can sit dark, held in
	 * reset or waiting in download mode — which looks exactly like a bricked
	 * device. Pulse EN with GPIO0 released so it always boots normally.
	 */
	private async releaseToRun(): Promise<void> {
		try {
			if (this.transport) {
				await this.transport.setDTR(false);
				await this.transport.setRTS(true);
				await sleep(100);
				await this.transport.setRTS(false);
				await sleep(50);
			}
		} catch {
			// signals unsupported or port already gone; nothing to do
		}
	}

	private async teardown(): Promise<void> {
		try {
			await this.transport?.disconnect();
		} catch {
			// port may already be gone
		}
		this.transport = null;
		this.loader = null;
	}

	async disconnect(): Promise<void> {
		await this.releaseToRun();
		await this.teardown();
	}
}
