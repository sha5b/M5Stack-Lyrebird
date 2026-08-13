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
const USB_FILTERS = [
	{ usbVendorId: 0x10c4, usbProductId: 0xea60 }, // Silicon Labs CP2102/CP2104 (M5Stack Fire)
	{ usbVendorId: 0x10c4, usbProductId: 0xea70 }, // Silicon Labs CP2105
	{ usbVendorId: 0x1a86, usbProductId: 0x7523 }, // WCH CH340
	{ usbVendorId: 0x1a86, usbProductId: 0x55d3 }, // WCH CH343
	{ usbVendorId: 0x1a86, usbProductId: 0x55d4 }, // WCH CH9102
	{ usbVendorId: 0x0403, usbProductId: 0x6001 } // FTDI FT232R
];

// Tried in order. Sync is always 115200; these are the post-sync rates. The
// last entry means "do not change baud at all", which is the most compatible.
const CONNECT_BAUD_RATES = [460800, 115200];

// esptool-js retries its sync internally and can sit for minutes without
// resolving, which is indistinguishable from a hang. Bound each attempt.
const ATTEMPT_TIMEOUT_MS = 20000;

function withTimeout<T>(work: Promise<T>, ms: number, label: string): Promise<T> {
	let timer: ReturnType<typeof setTimeout>;
	const expiry = new Promise<never>((_, reject) => {
		timer = setTimeout(() => reject(new Error(`${label} timed out after ${ms / 1000}s`)), ms);
	});
	return Promise.race([work, expiry]).finally(() => clearTimeout(timer)) as Promise<T>;
}

export interface FirmwareMeta {
	version: string;
	image: string;
	imageOffset: number;
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
	 * Sync always happens at 115200; the baud rate here is what the loader
	 * switches to afterwards. Some bridges are unreliable above ~460800, so a
	 * failure is retried at plain 115200 before giving up.
	 */
	async connect(onLog: (line: string) => void): Promise<DeviceInfo> {
		if (!this.port) throw new Error('No port selected');

		let lastError: unknown = null;
		for (const baudrate of CONNECT_BAUD_RATES) {
			try {
				onLog(`--- connecting at ${baudrate} baud`);
				return await withTimeout(
					this.attemptConnect(baudrate, onLog),
					ATTEMPT_TIMEOUT_MS,
					`Connect at ${baudrate}`
				);
			} catch (e) {
				lastError = e;
				onLog(`--- failed at ${baudrate}: ${e instanceof Error ? e.message : String(e)}`);
				await this.teardown();
			}
		}
		throw lastError instanceof Error ? lastError : new Error(String(lastError));
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
		meta: FirmwareMeta,
		image: Uint8Array,
		onProgress: (fraction: number) => void
	): Promise<void> {
		if (!this.loader) throw new Error('Not connected');

		await this.loader.writeFlash({
			fileArray: [{ data: image, address: meta.imageOffset }],
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
