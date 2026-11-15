import { useEffect, useState } from 'preact/hooks';
import { api, type PersonPreference } from './api';
import { log } from './diag';
let items: PersonPreference[] = [];
let ready = false;
let generation = 0;
const listeners = new Set<() => void>();
export function watchPreferences(fn: () => void): () => void { listeners.add(fn); return () => { listeners.delete(fn); }; }
export function preferences(): PersonPreference[] { return items; }
export function isBlocked(username: string): boolean { return items.some(p => p.username === username && p.blocked); }
export function isMuted(username: string): boolean { return items.some(p => p.username === username && (p.muted || p.blocked)); }
export function silentSource(username: string, source: string): boolean { return source === 'mic' ? isMuted(username) : isBlocked(username); }
export function preferencesReady(): boolean { return ready; }
function apply(next: PersonPreference[]) { items = next; ready = true; for (const fn of listeners) fn(); }
export async function refreshPreferences(): Promise<void> { const revision = ++generation; const next = await api.personPreferences(); if (revision === generation) apply(next.items); }
export async function changePreference(username: string, block: boolean, enabled: boolean): Promise<void> {
 ++generation;
 await api.personPreference(username, block, enabled);
 log('personal preference changed');
 await refreshPreferences();
}
export function usePreferences(): PersonPreference[] {
	const [snapshot, setSnapshot] = useState(items);
	useEffect(() => {
		// Catch a response that arrived between rendering and subscribing.
		setSnapshot(items);
		return watchPreferences(() => setSnapshot(items));
	}, []);
	return snapshot;
}
