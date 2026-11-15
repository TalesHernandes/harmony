// The one place in this client allowed to write a style, and it writes one
// property: the transform that puts a dragged picture where the pointer let it
// go, at the size its grip was pulled to (a scale, 24/09/2026 — the same
// property, so the exception did not grow).
//
// Everything else here is a class, and internal/harmony/api/security_test.go
// holds the line. This file is its only exception, granted by the owner on
// 24/09/2026 ("arrastar dentro da chamada" — anywhere, not only to a corner),
// and narrowed to exactly this: the test lets the transform assignment through here
// and nothing else, anywhere. A transform is also the cheapest thing a browser
// can move over a playing video: no layout, only compositing (CLAUDE.md,
// "animação só com transform").
//
// The page's CSP does not stand in the way: it forbids style *attributes* in
// markup, and a property set from script is not one.
export function place(el: HTMLElement, x: number, y: number, scale = 1): void {
	el.style.transform = `translate(${Math.round(x)}px, ${Math.round(y)}px) scale(${scale.toFixed(3)})`;
}
