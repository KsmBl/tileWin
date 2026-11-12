// What tilewin-portal runs in a login page that headless Chromium shows, when
// the page is made by scripts: the same idea as accept.c, on the live page.
// It ticks the checkboxes of the terms (not those of newsletters), presses
// the button that most says "let me in", and gives back what it did:
//   "clicked:<text>", "needs-input:<url>" or "nothing".
(() => {
	const IN = [
		['accept', 10], ['i accept', 12], ['accept terms', 12], ['accept and connect', 14],
		['agree', 10], ['i agree', 12], ['agree and connect', 14],
		['akzeptieren', 10], ['akzeptiere', 10], ['zustimmen', 10], ['ich stimme zu', 12],
		['einverstanden', 10], ['annehmen', 9], ['bestätigen', 7],
		['accepter', 10], ["j'accepte", 12], ['j’accepte', 12], ['aceptar', 10], ['acepto', 12],
		['accetta', 10], ['accetto', 12], ['accepteren', 10], ['akkoord', 10],
		['aceitar', 10], ['aceito', 12], ['akceptuj', 10], ['zgadzam się', 12], ['kabul', 10],
		['godkänn', 10], ['acceptera', 10],
		['connect', 7], ['connect now', 9], ['get online', 9], ['go online', 9],
		['start browsing', 9], ['start surfing', 9], ['free wifi', 6], ['free wi-fi', 6],
		['free internet', 6], ['access the internet', 8], ['continue', 6], ['proceed', 6],
		['log in', 5], ['login', 5], ['sign in', 4], ['start', 4], ['enter', 3], ['confirm', 6],
		['activate', 5], ['get started', 6],
		['verbinden', 7], ['jetzt verbinden', 9], ['weiter', 6], ['fortfahren', 6],
		['anmelden', 5], ['einloggen', 5], ['jetzt surfen', 9], ['lossurfen', 9],
		['kostenlos surfen', 9], ['ins internet', 8], ['freischalten', 7], ['starten', 4],
		['se connecter', 7], ['connexion', 6], ['continuer', 6], ['valider', 6],
		['conectar', 7], ['continuar', 6], ['entrar', 5], ['connetti', 7], ['continua', 6],
		['accedi', 5], ['doorgaan', 6], ['verder', 6], ['połącz', 7], ['dalej', 6],
		['bağlan', 7], ['devam', 6], ['anslut', 7], ['fortsätt', 6],
		['ok', 3], ['okay', 3], ['yes', 3], ['ja', 3], ['oui', 3], ['go', 3], ['submit', 3],
	];
	const OUT = [
		'decline', 'reject', 'refuse', 'disagree', 'do not agree', "don't agree", 'cancel',
		'back', 'abort', 'logout', 'log out', 'sign out', 'ablehnen', 'nicht einverstanden',
		'abbrechen', 'zurück', 'abmelden', 'refuser', 'annuler', 'retour', 'rechazar',
		'cancelar', 'volver', 'rifiuta', 'annulla', 'indietro', 'weigeren', 'annuleren',
		'terug', 'recusar', 'voltar', 'anuluj', 'odrzuć', 'reddet', 'iptal', 'avbryt',
		'more info', 'more information', 'learn more', 'read more', 'mehr erfahren',
		'mehr informationen', 'weitere informationen', 'details', 'help', 'hilfe',
		'language', 'sprache', 'privacy', 'privacy policy', 'datenschutz', 'impressum',
		'imprint', 'terms of use', 'terms and conditions', 'terms of service', 'agb',
		'nutzungsbedingungen', 'conditions', 'download', 'app', 'facebook', 'google', 'apple',
		'register', 'registrieren', 'sign up', 'create account', 'buy', 'kaufen', 'upgrade',
		'premium', 'pay', 'bezahlen', 'purchase', 'voucher', 'gutschein', 'ticket', 'code',
	];
	const MARKETING = [
		'newsletter', 'marketing', 'advert', 'werbung', 'werbe', 'angebote', 'offers',
		'promotions', 'promo', 'news', 'neuigkeiten', 'subscribe', 'abonnieren',
		'informiert werden', 'keep me informed', 'contact me', 'kontaktieren', 'e-mails',
		'emails', 'partner', 'third parties', 'dritte', 'publicité', 'offres', 'ofertas',
		'publicidad', 'remember me', 'angemeldet bleiben',
	];
	const DATA = [
		'mail', 'e-mail', 'email', 'name', 'vorname', 'nachname', 'surname', 'phone', 'tel',
		'mobile', 'handy', 'telefon', 'room', 'zimmer', 'chambre', 'voucher', 'gutschein',
		'code', 'ticket', 'pin', 'pass', 'password', 'passwort', 'kennwort', 'user',
		'username', 'benutzer', 'login', 'account', 'konto', 'birth', 'geburt', 'zip', 'plz',
		'postcode', 'address', 'adresse', 'card', 'booking', 'buchung', 'reservation',
		'membership', 'sms', 'otp', 'token',
	];

	const fold = (s) => (s || '').toLowerCase().replace(/\s+/g, ' ').trim();
	const escape = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
	const has = (text, phrase) =>
		new RegExp('(^|[^\\p{L}\\p{N}\'])' + escape(phrase) + '($|[^\\p{L}\\p{N}\'])', 'u').test(text);
	const score = (raw) => {
		const t = fold(raw);
		let s = 0;
		for (const [w, n] of IN) {
			if (n > s && has(t, w)) s = n;
		}
		if (OUT.some((w) => has(t, w))) s = s < 10 ? -20 : s - 3;
		if (t.length > 80 && s > 0) s = Math.floor(s / 2);
		return s;
	};

	// the page and the frames of it that can be reached
	const docs = [document];
	for (let i = 0; i < docs.length && i < 20; i++) {
		for (const frame of docs[i].querySelectorAll('iframe, frame')) {
			try {
				if (frame.contentDocument) docs.push(frame.contentDocument);
			} catch (e) { /* another origin */ }
		}
	}
	const all = (selector) => docs.flatMap((d) => [...d.querySelectorAll(selector)]);
	const visible = (el) => {
		const r = el.getBoundingClientRect();
		const st = el.ownerDocument.defaultView.getComputedStyle(el);
		return r.width > 0 && r.height > 0 && st.visibility !== 'hidden' && st.display !== 'none' &&
			st.opacity !== '0';
	};
	const textOf = (el) => fold(el.innerText || el.value || el.getAttribute('aria-label') ||
		el.title || el.alt || (el.querySelector && el.querySelector('img[alt]')?.alt) || '');
	const labelOf = (el) => {
		let t = '';
		for (const l of el.labels || []) t += ' ' + l.innerText;
		const around = el.closest('label');
		if (around) t += ' ' + around.innerText;
		t += ' ' + (el.name || '') + ' ' + (el.id || '') + ' ' + (el.getAttribute('aria-label') || '');
		if (!t.trim() && el.parentElement) t = el.parentElement.innerText;
		return fold(t);
	};
	// the part of the page a button belongs to: its form, else the nearest
	// box around it that holds fields
	const scopeOf = (el) => {
		if (el.form) return el.form;
		let box = el.parentElement;
		for (let i = 0; box && i < 5; i++, box = box.parentElement) {
			if (box.querySelector('input, textarea, select')) return box;
		}
		return el.ownerDocument.body || el.ownerDocument;
	};
	const wantsData = (scope) => {
		for (const el of scope.querySelectorAll('input, textarea')) {
			const type = (el.getAttribute('type') || 'text').toLowerCase();
			if (el.tagName === 'INPUT' && !['text', 'email', 'tel', 'number', 'password'].includes(type)) {
				continue;
			}
			if (!visible(el) || el.disabled || el.readOnly || el.value) continue;
			if (/^(q|s|search|query)$/i.test(el.name || '')) continue;
			const what = fold([el.name, el.id, el.placeholder, labelOf(el)].join(' '))
				.replace(/[_\-.[\]]/g, ' ');
			if (el.required || ['email', 'tel', 'password'].includes(type) ||
				DATA.some((w) => has(what, w) || (w.length >= 4 && what.includes(w)))) {
				return true;
			}
		}
		return false;
	};

	// the buttons, links and things with a role of button there are
	const candidates = [];
	for (const el of all('button, input[type=submit], input[type=button], input[type=image], ' +
		'a[href], [role=button], [onclick], .btn, .button, [class*="btn-"], [class*="button-"]')) {
		if (!visible(el) || el.disabled || el.getAttribute('aria-disabled') === 'true') continue;
		if (el.dataset && el.dataset.tilewinPressed) continue; // pressed before, nothing came of it
		const text = textOf(el);
		let s = score(text);
		if (el.tagName === 'A') {
			const href = (el.getAttribute('href') || '').trim().toLowerCase();
			const local = href.startsWith('#') || href.startsWith('javascript:') || href === '';
			if (s < 6 && !local) continue; // links lead elsewhere unless they say "in" strongly
		}
		if (s <= 0) continue;
		const scope = scopeOf(el);
		if (scope.querySelector && scope.querySelector('input[type=checkbox], [role=checkbox]')) s += 3;
		candidates.push({ el, text, s, scope });
	}
	candidates.sort((a, b) => b.s - a.s);
	const best = candidates.find((c) => c.s >= 3);
	if (!best) return 'nothing';
	if (wantsData(best.scope)) return 'needs-input:' + location.href;

	// the checkboxes of the terms, also those drawn over a hidden input
	for (const box of best.scope.querySelectorAll('input[type=checkbox]')) {
		if (box.checked || box.disabled || MARKETING.some((w) => labelOf(box).includes(w))) continue;
		if (visible(box)) {
			box.click();
		} else {
			const label = [...(box.labels || [])].find(visible) || box.closest('label');
			if (label) label.click();
		}
		if (!box.checked) {
			box.checked = true;
			box.dispatchEvent(new Event('input', { bubbles: true }));
			box.dispatchEvent(new Event('change', { bubbles: true }));
		}
	}
	for (const box of best.scope.querySelectorAll('[role=checkbox][aria-checked=false]')) {
		if (visible(box) && !MARKETING.some((w) => fold(box.innerText).includes(w))) box.click();
	}

	best.el.dataset.tilewinPressed = '1';
	best.el.scrollIntoView({ block: 'center' });
	for (const type of ['pointerdown', 'mousedown', 'pointerup', 'mouseup']) {
		best.el.dispatchEvent(new MouseEvent(type, { bubbles: true, cancelable: true, view: window }));
	}
	best.el.click();
	return 'clicked:' + best.text.slice(0, 80);
})()
