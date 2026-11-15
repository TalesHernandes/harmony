// NetworkInformation is optional (Firefox does not expose it).
export function watchNetwork(changed: (reason: string) => void): () => void {
	const online = () => changed('online');
	const offline = () => changed('offline');
	const connection = (navigator as Navigator & { connection?: EventTarget & { type?: string } }).connection;
	let previousType = connection?.type;
	const change = () => {
		const type = connection?.type;
		const previous = previousType;
		previousType = type;
		// Estimates of bandwidth/latency also emit change. Only a known
		// interface change warrants restarting a healthy media connection.
		if (type && previous && type !== previous) changed('interface');
	};
	window.addEventListener('online', online);
	window.addEventListener('offline', offline);
	connection?.addEventListener('change', change);
	return () => {
		window.removeEventListener('online', online);
		window.removeEventListener('offline', offline);
		connection?.removeEventListener('change', change);
	};
}
