import { render } from 'preact';
import { useEffect, useState } from 'preact/hooks';
import { api, ApiError, type User } from './api';
import { EmailVerification, ResetPassword } from './Security';
import './style.css';
import { App } from './App';
import { Login, Signup } from './Auth';
import { log } from './diag';
import { startScrollbars } from './scrollbar';

// One document, three screens. Which one is drawn comes from the address bar;
// which one a visitor is allowed to reach was already decided by the server
// before this file was served (internal/harmony/api/pages.go).
const resetToken = new URLSearchParams(location.hash.slice(1)).get('reset');
if (resetToken !== null) history.replaceState(null, '', location.pathname + location.search);

// The app starts at once, in parallel with the question "is this address
// confirmed?", instead of after it: a round trip in front of every opening is
// a wait nobody sees the reason for. Almost everybody is confirmed and never
// notices the gate; the rest swap screens when the answer lands. Any other
// failure is the app's to report, as it always was.
function SessionGate() {
	const [who, setWho] = useState<{ user: User; email_required: boolean } | null>(null);
	const [skipped, setSkipped] = useState(false);
	useEffect(() => { void api.me().then(setWho).catch((err) => {
		if (err instanceof ApiError && err.status === 401) location.href = '/login';
	}); }, []);
	if (who && !who.user.email_verified && !skipped) return <EmailVerification user={who.user} required={who.email_required} onDone={() => location.reload()} onSkip={() => setSkipped(true)} />;
	return <App />;
}
function Root() {
	if (resetToken !== null) return <ResetPassword token={resetToken} />;
	const path = location.pathname;
	if (path === '/login') return <Login />;
	if (path === '/signup') return <Signup />;
	return <SessionGate />;
}

log('cliente carregado');

const root = document.getElementById('app');
if (root) render(<Root />, root);
startScrollbars();
