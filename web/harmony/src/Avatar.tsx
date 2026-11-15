// A face: the person's initials in a circle, and — where there is a presence to
// report — the state dot cut *out* of that circle instead of laid on top of it.
//
// The hole is the whole reason this is a component and not two spans copied
// into three lists. A dot drawn over the edge of a face needs a ring of the
// background colour around it to stay legible, and that ring is a lie the
// moment the row behind it changes colour: selected, under the pointer, or the
// drawer's darker panel. Punching the hole out of the face with a mask costs
// one CSS rule, is right on every background there will ever be, and is what
// the design does ("harmony - Etapa 2", the channel column and the people
// column both).
//
// The mask lives in style.css because it is geometry, not a colour: a hole at
// 85.4% of the way across and down, which is the point where the circle's own
// edge passes at 45 degrees. Its radius is the dot's radius plus two pixels of
// gap, so the gap exists without anything being drawn to make it — which is why
// there are two rules and not one, and why the dot and the rule below have to
// be changed in the same breath.

import { initials } from './Call';
import { useState } from 'preact/hooks';
import { dotClass, type PersonState } from './presence';

export function Avatar({
	name,
	username = '',
	avatar = '',
	state,
	speaking = false,
	size = 32,
}: {
	name: string;
	username?: string;
	avatar?: string;
	// No state at all means no hole and no dot. That is the face under a voice
	// room, where being in the room is the presence and a second green dot
	// beside the green ring would be the same news twice.
	state?: PersonState;
	// Talking right now. A ring drawn *inside* the circle, so somebody starting
	// to speak does not move the two people listed below them by a pixel.
	speaking?: boolean;
	// 32 is a row in a list of people or conversations; 28 is a name tucked
	// under a voice room, where the face is a marker beside a name rather than
	// the thing the row is about.
	size?: 28 | 32 | 36;
}) {
	// The box is set once, on the wrapper; the inner span only says how big the
	// initials are. Saying h-8 in both places works until the day Tailwind emits
	// h-full after h-8 and the face quietly changes size.
	const small = size === 28;
	const url = avatar && username ? avatarURL(username, avatar) : '';
	const [failed, setFailed] = useState('');
	const quiet = state === 'offline';
	return (
		<span class={'relative block shrink-0 ' + (small ? 'h-7 w-7' : size === 36 ? 'h-9 w-9' : 'h-8 w-8')} data-face>
			<span
				class={
					'grid h-full w-full place-items-center rounded-full font-semibold ' +
					(small ? 'text-label' : 'text-meta') +
					(quiet ? ' bg-surface-2 text-foreground-muted' : ' bg-surface-3 text-foreground') +
					(state ? (small ? ' avatar-hole' : ' avatar-hole-lg') : '') +
					(speaking ? ' ring-2 ring-inset ring-success' : '')
				}
			>
				{url && failed !== url ? (
					<img
						src={url}
						class={'h-full w-full rounded-full object-cover' + (speaking ? ' border-2 border-success' : '')}
						loading="lazy"
						alt=""
						onError={() => setFailed(url)}
					/>
				) : (
					initials(name)
				)}
			</span>
			{state && (
				<span
					class={
						'avatar-dot rounded-full ' +
						(small ? 'h-2 w-2 ' : 'h-2.5 w-2.5 ') +
						dotClass(state)
					}
				/>
			)}
		</span>
	);
}

// The inside of a circle someone else drew and sized (a call's big circles):
// the photo filling it when the person has one, else the initials it held
// before. The circle keeps its own size and colour; the photo just covers it.
export function FaceFill({ name, username, avatar }: { name: string; username: string; avatar?: string }) {
	const url = avatar && username ? avatarURL(username, avatar) : '';
	const [failed, setFailed] = useState('');
	return url && failed !== url ? (
		<img src={url} class="h-full w-full rounded-full object-cover" alt="" onError={() => setFailed(url)} />
	) : (
		<>{initials(name)}</>
	);
}

export function avatarURL(username: string, version: string): string {
	return `/api/people/${encodeURIComponent(username)}/avatar?v=${encodeURIComponent(version)}`;
}
