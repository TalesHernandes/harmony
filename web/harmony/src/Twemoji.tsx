// Emoji drawn as Twemoji pictures instead of the system's own font.
//
// The owner asked for them to look like Apple's (24/09/2026), and Apple's own
// set cannot be shipped by anybody but Apple. What can be done is the next
// best thing, and the part of the request that was really about looking right:
// the same drawing on every screen, so the fire somebody sends from an iPhone
// is the fire that arrives on a Windows PC, and not Microsoft's flat outline
// of it. Twemoji is the open set Discord itself draws with (option 9A).
//
// The pictures are copied into the build by vite.config.ts and served from this
// same origin. Nothing here makes the message markup: the body is still text
// nodes, with an <img> put where an emoji was, and the <img> carries the emoji
// itself as its alt, so copying the message copies the emoji.

import { useState } from 'preact/hooks';

// One emoji as a person sees it: a flag (two regional letters), a keycap, or a
// pictograph with its variation selector, skin tone, tag run (the English and
// Scottish flags) and any number of zero-width-joined parts after it (a family,
// a firefighter, a heart on fire).
const PART = '\\p{Extended_Pictographic}(?:\\uFE0F|\\p{Emoji_Modifier})?[\\u{E0020}-\\u{E007F}]*';
const SOURCE =
	'\\p{Regional_Indicator}{2}|[#*0-9]\\uFE0F?\\u20E3|' + PART + '(?:\\u200D' + PART + ')*';

// A fresh expression per call: a /g one shared across renders keeps lastIndex
// between them, the trap the mention matcher in Messages.tsx resets by hand.
const emojiRE = () => new RegExp(SOURCE, 'gu');

// Characters that are pictographs to Unicode but letters to everybody reading
// them - ©, ®, ™, ‼ and the rest default to text. They only become a picture
// when the sender's keyboard asked for one with U+FE0F, the way Twemoji itself
// decides it.
const TEXT_BY_DEFAULT = /^\p{Extended_Pictographic}$/u;
const PRESENTS_AS_EMOJI = /\p{Emoji_Presentation}/u;

function isPicture(match: string): boolean {
	if (TEXT_BY_DEFAULT.test(match) && !PRESENTS_AS_EMOJI.test(match)) return false;
	return true;
}

// The picture's file name is the code points in hex, joined by "-". U+FE0F is
// dropped unless the emoji is a joined one, which is Twemoji's own rule for
// naming its files ("2764" for a red heart, "2764-fe0f-200d-1f525" for one on
// fire).
export function emojiFile(emoji: string): string {
	const points = [...emoji].map((c) => c.codePointAt(0)!);
	const joined = points.includes(0x200d);
	return points
		.filter((p) => joined || p !== 0xfe0f)
		.map((p) => p.toString(16))
		.join('-');
}

export const TWEMOJI_BASE = '/static/assets/twemoji-15/';

// Emoji is one picture. If this version of Twemoji has no drawing for it (an
// emoji newer than the set), the character itself comes back in its place and
// the system draws it, which is what every emoji looked like before.
//
// `hold` keeps the place without fetching anything: the emoji panel is 1,867
// pictures, and loading="lazy" measures against the window, not the panel's
// own list, so every one of them was fetched the first time it opened - 8 MB
// on a telephone. The panel says which groups are near enough to show.
export function Emoji({ char, class: extra = '', hold = false }: { char: string; class?: string; hold?: boolean }) {
	const [failed, setFailed] = useState(false);
	if (failed) return <>{char}</>;
	if (hold) return <span class={'emoji emoji-hold ' + extra} aria-hidden="true" />;
	return (
		<img
			class={'emoji ' + extra}
			src={TWEMOJI_BASE + emojiFile(char) + '.svg'}
			alt={char}
			draggable={false}
			loading="lazy"
			data-twemoji={char}
			onError={() => setFailed(true)}
		/>
	);
}

// withEmoji is a run of text with every emoji in it turned into a picture.
export function withEmoji(text: string): any[] {
	const out: any[] = [];
	let last = 0;
	const re = emojiRE();
	for (let m = re.exec(text); m; m = re.exec(text)) {
		if (!isPicture(m[0])) continue;
		if (m.index > last) out.push(text.slice(last, m.index));
		out.push(<Emoji key={m.index} char={m[0]} />);
		last = m.index + m[0].length;
	}
	if (last === 0) return [text];
	if (last < text.length) out.push(text.slice(last));
	return out;
}
