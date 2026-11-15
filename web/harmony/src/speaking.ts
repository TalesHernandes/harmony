// Linear RMS, before playback gain. -60 dBFS is a low fixed floor;
// the denoiser's residual floor has not been measured on the owner's device.
export const SPEAKING_FLOOR = 0.001;
export const SPEAKING_HOLD_MS = 300;

/** One level sample: immediate attack, release after 300 ms of silence. */
export function speakingSample(lastAbove: number | undefined, level: number, now: number): number | undefined {
	if (Number.isFinite(level) && level > SPEAKING_FLOOR) return now;
	return lastAbove !== undefined && now - lastAbove < SPEAKING_HOLD_MS ? lastAbove : undefined;
}
