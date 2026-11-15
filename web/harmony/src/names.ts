// What each person is called, here.
//
// A nickname is one person's private name for another: you set it, only you are
// ever shown it, and the server enforces that by shape rather than by rule —
// every row in the table carries whose opinion it is, so there is no query that
// could hand somebody else's to you (store migration 7).
//
// It lives in a module of its own, set once when the list of people arrives,
// because otherwise it would have to be threaded through every component that
// draws a name — the column, the conversation, the call, the profile, the line
// that says who is typing. A name is not state that belongs to any one of them.
//
// It is deliberately *not* in the browser's storage, where the volumes are. A
// volume is an opinion about a machine — this laptop's speakers, this pair of
// headphones — and belongs to the machine. A name is an opinion about a person
// and has to be the same on the telephone as on the desktop.

let nicknames: Record<string, string> = {};

export function setNicknames(next: Record<string, string>): void {
	nicknames = next;
}

// nameOf is what to draw. The nickname if there is one, otherwise whatever the
// person calls themselves, otherwise their username — which always exists.
export function nameOf(username: string, theirOwnName = ''): string {
	return nicknames[username] || theirOwnName || username;
}

// nicknameOf is the empty string when there is none, which is what the box for
// setting one wants to show.
export function nicknameOf(username: string): string {
	return nicknames[username] ?? '';
}
