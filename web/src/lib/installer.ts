import type { ESPLoader, Transport } from 'esptool-js';

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
/**
 * Espressif's VID, and the one product id that is a ROM bootloader.
 *
 * An ESP32-S3 board with native USB is two different USB devices depending on
 * what it is running. The CoreS3's application presents its own CDC — 0x303a
 * with an M5Stack product id such as 0x811a — while the ROM's USB-Serial-JTAG
 * is 0x303a:0x1001. Only the second one can be flashed.
 *
 * This is not a detail we can paper over. On a board with a UART bridge the
 * flasher resets the chip into its bootloader over DTR/RTS and the USB device
 * stays put. Here the reset *replaces* the USB device, so the handle the browser
 * granted stops existing mid-connect — which is why the CoreS3 appeared to
 * restart endlessly and never answer. The board has to be in download mode
 * before the port is picked, and then it is a different entry in the picker.
 */
export const ESPRESSIF_VID = 0x303a;
const ROM_BOOTLOADER_PID = 0x1001;

export function isEspressifAppPort(ids: { vid: number; pid: number } | null): boolean {
	return ids !== null && ids.vid === ESPRESSIF_VID && ids.pid !== ROM_BOOTLOADER_PID;
}

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
	{ usbVendorId: ESPRESSIF_VID }
];

// Sync always happens at 115200 (esptool-js `romBaudrate`); these are the rates
// the loader switches to *after* sync. Some bridges are unreliable above ~460800.
const FAST_BAUD = 460800;
const SLOW_BAUD = 115200;

/**
 * There is deliberately no timeout around the connect, and that is a fix rather
 * than an omission.
 *
 * A timeout here cannot cancel anything. Rejecting the race leaves esptool-js's
 * own promise running: it keeps working through its seven attempts, and it keeps
 * the serial port open while it does. The port then stays claimed after we have
 * reported failure and torn down, so the next attempt — and any other program,
 * including esptool.py in a terminal — gets "the port is busy or doesn't exist".
 * That is a worse failure than the hang it was guarding against, and harder to
 * recognise, because nothing on screen says the previous attempt is still going.
 *
 * It is also unnecessary: `ESPLoader.connect()` is already bounded. Seven
 * attempts, each a reset sequence and five sync reads with a 100 ms timeout
 * apiece, so it settles in well under a minute and then throws by itself.
 */

/**
 * A note on the reset sequence, so nobody re-breaks this.
 *
 * esptool-js's ClassicReset is `D0|R1|W100|D1|R0|W50|D0`, and an earlier version
 * of this file replaced it with "improved" sequences that held EN low longer and
 * inserted a settling wait between pulling IO0 low and releasing EN. Measured on
 * an M5Stack Fire, the stock sequence entered download mode 3/3 and both
 * replacements 0/3. They were a straight regression.
 *
 * The reasoning behind them was wrong about the hardware. This is the classic
 * two-transistor auto-reset circuit, where EN and IO0 are driven by the
 * *combination* of DTR and RTS rather than one line each:
 *
 *     DTR RTS -> EN IO0
 *      0   1      0   1     reset asserted
 *      1   1      1   1     reset released, IO0 still high
 *      1   0      1   0     IO0 low
 *
 * So in `D1|R0` it is `D1` that releases EN and `R0` that pulls IO0 low — the
 * opposite of the assumption. A wait between them gives the chip time to latch
 * IO0 high and boot the application, which is exactly the failure it was meant
 * to prevent. Longer waits elsewhere are harmless (250 ms on either side also
 * measured 4/4), but they buy nothing, so the library's own sequence stands.
 */

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
	 * Drop the granted port so the next connect re-opens the picker.
	 *
	 * A Web Serial port is a handle to one USB device, and several things here
	 * invalidate it: an ESP32-S3 leaving its application for its ROM bootloader
	 * swaps one USB device for another, and a board that browns out or is
	 * unplugged takes its device with it. In both cases the handle stays non-null
	 * and every retry fails against something that no longer exists, which reads
	 * as "the board will not connect" when the truth is "we are talking to a
	 * ghost". Cheaper to ask for the port again than to explain that.
	 */
	forgetPort(): void {
		this.port = null;
	}

	/** VID/PID of the chosen port, for the download-mode check below. */
	portIds(): { vid: number; pid: number } | null {
		if (!this.port) return null;
		const info = this.port.getInfo();
		if (info.usbVendorId === undefined || info.usbProductId === undefined) return null;
		return { vid: info.usbVendorId, pid: info.usbProductId };
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
			return await this.attemptConnect(FAST_BAUD, onLog);
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
			return await this.attemptConnect(SLOW_BAUD, onLog);
		}
	}

	private async attemptConnect(
		baudrate: number,
		onLog: (line: string) => void
	): Promise<DeviceInfo> {
		// Loaded on demand: esptool-js ships extensionless ESM imports that
		// Node cannot resolve during prerendering, and it is only ever needed
		// in the browser once the user has actually picked a device.
		const { ESPLoader, Transport } = await import('esptool-js');

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
