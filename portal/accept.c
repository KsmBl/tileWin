#define _GNU_SOURCE
#include <ctype.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "accept.h"

/* ---------- words ---------- */

struct word {
	const char *text;
	int weight;
};

/*
 * Lower case, as html_fold makes the text. A phrase matches as whole words:
 * "ok" is not found in "book", nor "go" in "google".
 */
static const struct word words_in[] = {
	// saying yes to the terms: the strongest
	{ "accept", 10 }, { "i accept", 12 }, { "accept terms", 12 }, { "accept and connect", 14 },
	{ "agree", 10 }, { "i agree", 12 }, { "agree and connect", 14 }, { "i have read", 4 },
	{ "akzeptieren", 10 }, { "akzeptiere", 10 }, { "zustimmen", 10 }, { "ich stimme zu", 12 },
	{ "einverstanden", 10 }, { "annehmen", 9 }, { "bestätigen", 7 }, { "bestaetigen", 7 },
	{ "accepter", 10 }, { "j'accepte", 12 }, { "j’accepte", 12 }, { "accepte", 10 },
	{ "aceptar", 10 }, { "acepto", 12 }, { "accetta", 10 }, { "accetto", 12 },
	{ "accettare", 10 }, { "accepteren", 10 }, { "akkoord", 10 }, { "ik ga akkoord", 12 },
	{ "aceitar", 10 }, { "aceito", 12 }, { "concordo", 10 }, { "akceptuj", 10 },
	{ "akceptuję", 12 }, { "zgadzam się", 12 }, { "kabul", 10 }, { "kabul ediyorum", 12 },
	{ "godkänn", 10 }, { "acceptera", 10 }, { "jag godkänner", 12 },
	// getting on the internet
	{ "connect", 7 }, { "connect now", 9 }, { "get online", 9 }, { "go online", 9 },
	{ "start browsing", 9 }, { "start surfing", 9 }, { "free wifi", 6 }, { "free wi-fi", 6 },
	{ "free internet", 6 }, { "internet access", 5 }, { "access the internet", 8 },
	{ "continue", 6 }, { "proceed", 6 }, { "log in", 5 }, { "login", 5 }, { "sign in", 4 },
	{ "start", 4 }, { "enter", 3 }, { "confirm", 6 }, { "activate", 5 }, { "get started", 6 },
	{ "verbinden", 7 }, { "jetzt verbinden", 9 }, { "verbindung herstellen", 8 },
	{ "weiter", 6 }, { "fortfahren", 6 }, { "anmelden", 5 }, { "einloggen", 5 },
	{ "jetzt surfen", 9 }, { "lossurfen", 9 }, { "los geht's", 7 }, { "los gehts", 7 },
	{ "kostenlos surfen", 9 }, { "ins internet", 8 }, { "freischalten", 7 }, { "starten", 4 },
	{ "se connecter", 7 }, { "connexion", 6 }, { "continuer", 6 }, { "valider", 6 },
	{ "conectar", 7 }, { "conectarse", 7 }, { "continuar", 6 }, { "entrar", 5 },
	{ "connetti", 7 }, { "connettiti", 7 }, { "continua", 6 }, { "accedi", 5 },
	{ "doorgaan", 6 }, { "verder", 6 }, { "inloggen", 5 }, { "połącz", 7 }, { "dalej", 6 },
	{ "bağlan", 7 }, { "devam", 6 }, { "anslut", 7 }, { "fortsätt", 6 },
	// a plain button
	{ "ok", 3 }, { "okay", 3 }, { "yes", 3 }, { "ja", 3 }, { "oui", 3 }, { "go", 3 },
	{ "submit", 3 }, { "senden", 2 }, { "absenden", 3 }, { "envoyer", 2 }, { "enviar", 2 },
};

/* The ways out, and the buttons that lead elsewhere. */
static const char *const words_out[] = {
	"decline", "reject", "refuse", "disagree", "do not agree", "don't agree", "cancel",
	"back", "abort", "logout", "log out", "sign out", "ablehnen", "nicht einverstanden",
	"abbrechen", "zurück", "abmelden", "refuser", "annuler", "retour", "rechazar",
	"cancelar", "volver", "rifiuta", "annulla", "indietro", "weigeren", "annuleren",
	"terug", "recusar", "voltar", "anuluj", "odrzuć", "reddet", "iptal", "avbryt",
	"more info", "more information", "learn more", "read more", "mehr erfahren",
	"mehr informationen", "weitere informationen", "details", "help", "hilfe",
	"language", "sprache", "english", "deutsch", "français", "español", "italiano",
	"privacy", "privacy policy", "datenschutz", "datenschutzerklärung", "impressum",
	"imprint", "terms of use", "terms and conditions", "terms of service", "agb",
	"nutzungsbedingungen", "conditions", "download", "app", "facebook", "google",
	"apple", "twitter", "instagram", "register", "registrieren", "sign up",
	"create account", "konto erstellen", "buy", "kaufen", "upgrade", "premium",
	"pay", "bezahlen", "purchase", "voucher", "gutschein", "ticket", "code",
};

/* Checkboxes that stay unticked: newsletters, offers, being contacted. */
static const char *const words_marketing[] = {
	"newsletter", "marketing", "advert", "advertising", "werbung", "werbe", "angebote",
	"offers", "promotions", "promo", "special offers", "news", "neuigkeiten",
	"subscribe", "abonnieren", "informiert werden", "keep me informed", "contact me",
	"kontaktieren", "e-mails", "emails", "partner", "third parties", "dritte",
	"publicité", "offres", "ofertas", "publicidad", "remember me", "angemeldet bleiben",
};

/* Fields that ask for something of the person: never made up. */
static const char *const words_data[] = {
	"mail", "e-mail", "email", "name", "vorname", "nachname", "first", "last", "surname",
	"phone", "tel", "mobile", "handy", "telefon", "room", "zimmer", "chambre",
	"habitación", "voucher", "gutschein", "code", "ticket", "pin", "pass", "password",
	"passwort", "kennwort", "user", "username", "benutzer", "login", "account", "konto",
	"birth", "geburt", "age", "alter", "zip", "plz", "postcode", "address", "adresse",
	"card", "karte", "booking", "buchung", "reservation", "membership", "mitglied",
	"sms", "otp", "token", "key", "schlüssel", "id",
};

static bool is_word_char(unsigned char c) {
	return isalnum(c) || c >= 0x80 || c == '\'' || c == 0xE2;
}

/* Whether the folded text holds the phrase as whole words. */
static bool has_phrase(const char *text, const char *phrase) {
	size_t n = strlen(phrase);
	for (const char *p = strstr(text, phrase); p; p = strstr(p + 1, phrase)) {
		bool start = p == text || !is_word_char((unsigned char)p[-1]);
		bool end = !is_word_char((unsigned char)p[n]);
		if (start && end) {
			return true;
		}
	}
	return false;
}

/* Whether any of the words is part of the text, also inside a longer word. */
static bool has_part(const char *text, const char *const *list, size_t count) {
	for (size_t i = 0; i < count; i++) {
		if (strstr(text, list[i])) {
			return true;
		}
	}
	return false;
}

int portal_text_score(const char *text) {
	char *t = html_fold(text);
	int score = 0;
	for (size_t i = 0; i < sizeof(words_in) / sizeof(words_in[0]); i++) {
		if (has_phrase(t, words_in[i].text) && words_in[i].weight > score) {
			score = words_in[i].weight;
		}
	}
	bool out = false;
	for (size_t i = 0; i < sizeof(words_out) / sizeof(words_out[0]) && !out; i++) {
		out = has_phrase(t, words_out[i]);
	}
	// "accept the terms of use" says yes; "terms of use" alone leads to them
	if (out && score < 10) {
		score = -20;
	} else if (out) {
		score -= 3;
	}
	// a long text is a paragraph, not a button
	if (strlen(t) > 80 && score > 0) {
		score /= 2;
	}
	free(t);
	return score;
}

bool portal_is_marketing(const char *text) {
	char *t = html_fold(text);
	bool yes = has_part(t, words_marketing, sizeof(words_marketing) / sizeof(words_marketing[0]));
	free(t);
	return yes;
}

/* Whether a text field asks for something of the person. */
static bool asks_data(const struct html_field *f) {
	if (strcmp(f->type, "password") == 0 || strcmp(f->type, "email") == 0 ||
			strcmp(f->type, "tel") == 0) {
		return true;
	}
	char *texts[3] = { html_fold(f->name), html_fold(f->id), html_fold(f->label) };
	bool yes = false;
	for (int i = 0; i < 3 && !yes; i++) {
		// whole words of a name like "guest_email" or "roomNumber"
		for (char *c = texts[i]; *c; c++) {
			if (*c == '_' || *c == '-' || *c == '.' || *c == '[' || *c == ']') {
				*c = ' ';
			}
		}
		for (size_t w = 0; w < sizeof(words_data) / sizeof(words_data[0]) && !yes; w++) {
			yes = has_phrase(texts[i], words_data[w]) ||
				(strlen(words_data[w]) >= 4 && strstr(texts[i], words_data[w]));
		}
	}
	for (int i = 0; i < 3; i++) {
		free(texts[i]);
	}
	return yes;
}

static bool is_search(const struct html_field *f) {
	if (strcmp(f->type, "search") == 0) {
		return true;
	}
	const char *n = f->name ? f->name : "";
	return strcmp(n, "q") == 0 || strcmp(n, "s") == 0 || strcasecmp(n, "search") == 0 ||
		strcasecmp(n, "query") == 0;
}

char *portal_resolve(const char *base, const char *href) {
	if (!href) {
		return NULL;
	}
	while (isspace((unsigned char)*href)) {
		href++;
	}
	if (strncasecmp(href, "javascript:", 11) == 0 || strncasecmp(href, "mailto:", 7) == 0 ||
			strncasecmp(href, "tel:", 4) == 0 || strncasecmp(href, "data:", 5) == 0 ||
			href[0] == '#') {
		return NULL;
	}
	CURLU *u = curl_url();
	char *out = NULL;
	if (u && (!base || curl_url_set(u, CURLUPART_URL, base, 0) == CURLUE_OK) &&
			curl_url_set(u, CURLUPART_URL, href[0] ? href : (base ? base : ""),
				CURLU_NON_SUPPORT_SCHEME) == CURLUE_OK) {
		curl_url_set(u, CURLUPART_FRAGMENT, NULL, 0);
		char *url = NULL;
		if (curl_url_get(u, CURLUPART_URL, &url, 0) == CURLUE_OK) {
			if (strncasecmp(url, "http://", 7) == 0 || strncasecmp(url, "https://", 8) == 0) {
				out = strdup(url);
			}
			curl_free(url);
		}
	}
	curl_url_cleanup(u);
	return out;
}

/* ---------- forms ---------- */

static void add_pair(struct portal_action *a, const char *name, const char *value) {
	a->names = realloc(a->names, sizeof(char *) * (size_t)(a->npairs + 1));
	a->values = realloc(a->values, sizeof(char *) * (size_t)(a->npairs + 1));
	a->names[a->npairs] = strdup(name);
	a->values[a->npairs] = strdup(value ? value : "");
	a->npairs++;
}

/* What the form sends when that button is pressed (-1: none, as by Enter). */
static void form_pairs(struct portal_action *a, const struct html_form *form, int button) {
	for (int i = 0; i < form->nfields; i++) {
		const struct html_field *f = &form->fields[i];
		if (!f->name || !f->name[0] || f->disabled) {
			continue;
		}
		switch (f->kind) {
		case HTML_HIDDEN:
		case HTML_TEXT:
		case HTML_TEXTAREA:
		case HTML_SELECT:
			add_pair(a, f->name, f->value);
			break;
		case HTML_CHECKBOX:
			if (!portal_is_marketing(f->label) && !portal_is_marketing(f->name)) {
				add_pair(a, f->name, f->value);
			}
			break;
		case HTML_RADIO: {
			// of its group the one checked, else the first
			int chosen = -1;
			for (int j = 0; j < form->nfields; j++) {
				const struct html_field *g = &form->fields[j];
				if (g->kind != HTML_RADIO || !g->name || strcmp(g->name, f->name) != 0) {
					continue;
				}
				if (g->checked) {
					chosen = j;
					break;
				}
				if (chosen < 0) {
					chosen = j;
				}
			}
			if (chosen == i) {
				add_pair(a, f->name, f->value);
			}
			break;
		}
		case HTML_SUBMIT:
			if (i == button) {
				add_pair(a, f->name, f->value);
			}
			break;
		case HTML_IMAGE:
			if (i == button) {
				char name[512];
				snprintf(name, sizeof(name), "%s.x", f->name);
				add_pair(a, name, "1");
				snprintf(name, sizeof(name), "%s.y", f->name);
				add_pair(a, name, "1");
			}
			break;
		default:
			break;
		}
	}
	if (button >= 0 && form->fields[button].kind == HTML_IMAGE &&
			(!form->fields[button].name || !form->fields[button].name[0])) {
		add_pair(a, "x", "1");
		add_pair(a, "y", "1");
	}
}

struct candidate {
	int form, button, score;
	bool needs_input;
};

/* How good a way in the form is, pressed with that button. */
static struct candidate rate_form(const struct html_form *form, int index) {
	struct candidate best = { index, -1, -100, false };
	bool checkbox = false, search = false, data = false;
	int text_fields = 0;
	for (int i = 0; i < form->nfields; i++) {
		const struct html_field *f = &form->fields[i];
		if (f->kind == HTML_CHECKBOX && !f->invisible) {
			checkbox = true;
		}
		if (f->kind == HTML_TEXT && !f->invisible && !f->disabled) {
			text_fields++;
			if (is_search(f)) {
				search = true;
			} else if (!f->value[0] && (f->required || asks_data(f))) {
				data = true;
			}
		}
		if (f->kind == HTML_TEXTAREA && !f->invisible && f->required && !f->value[0]) {
			data = true;
		}
	}
	if (search && text_fields == 1 && !checkbox) {
		return best; // a search box
	}
	int bonus = (checkbox ? 3 : 0) + (portal_text_score(form->text) > 0 ? 1 : 0);
	for (int i = 0; i < form->nfields; i++) {
		const struct html_field *f = &form->fields[i];
		if ((f->kind != HTML_SUBMIT && f->kind != HTML_IMAGE) || f->disabled) {
			continue;
		}
		const char *text = f->label[0] ? f->label : f->value;
		int score = portal_text_score(text);
		if (score < 0) {
			continue;
		}
		// an unnamed lone button still sends the form
		if (score == 0) {
			score = 2;
		}
		score += bonus;
		if (score > best.score) {
			best.score = score;
			best.button = i;
		}
	}
	if (best.button < 0) {
		// no button: sent by a script, or by Enter; the text around has to say so
		int score = portal_text_score(form->text);
		if (score > 0 || checkbox) {
			best.score = (score > 0 ? score / 2 : 0) + bonus - 1;
		}
	}
	best.needs_input = data;
	return best;
}

static void describe(struct portal_action *a, const char *kind, const char *text) {
	char what[160];
	snprintf(what, sizeof(what), "%s '%.100s'", kind, text && text[0] ? text : "(no text)");
	free(a->what);
	a->what = strdup(what);
}

struct portal_action portal_decide(const struct html_page *page, const char *base_url) {
	struct portal_action a = { .step = PORTAL_NOTHING };
	struct candidate best = { -1, -1, 0, false }, wants = { -1, -1, 0, false };
	for (int i = 0; i < page->nforms; i++) {
		struct candidate c = rate_form(&page->forms[i], i);
		if (c.needs_input) {
			if (c.score > wants.score) {
				wants = c;
			}
		} else if (c.score > best.score) {
			best = c;
		}
	}
	int link = -1, link_score = 0;
	for (int i = 0; i < page->nlinks; i++) {
		int score = portal_text_score(page->links[i].text);
		char *url = portal_resolve(base_url, page->links[i].href);
		if (url && score > link_score) {
			link_score = score;
			link = i;
		}
		free(url);
	}
	// a form needs a word that says "in" or a checkbox; a link a strong word
	bool form_ok = best.form >= 0 && best.score >= 3;
	bool link_ok = link >= 0 && link_score >= 6;
	if (form_ok && (!link_ok || best.score >= link_score)) {
		const struct html_form *form = &page->forms[best.form];
		const struct html_field *button = best.button >= 0 ? &form->fields[best.button] : NULL;
		const char *action = button && button->formaction ? button->formaction : form->action;
		const char *method = button && button->formmethod ? button->formmethod : form->method;
		a.url = portal_resolve(base_url, action && action[0] ? action : base_url);
		if (!a.url) {
			a.url = strdup(base_url);
		}
		a.post = method && strcasecmp(method, "post") == 0;
		a.multipart = a.post && form->enctype &&
			strcasecmp(form->enctype, "multipart/form-data") == 0;
		form_pairs(&a, form, best.button);
		a.step = PORTAL_SUBMIT;
		a.score = best.score;
		if (button) {
			describe(&a, "the button", button->label[0] ? button->label : button->value);
		} else {
			describe(&a, "the form", form->text);
		}
		return a;
	}
	if (link_ok) {
		a.step = PORTAL_LINK;
		a.url = portal_resolve(base_url, page->links[link].href);
		a.score = link_score;
		describe(&a, "the link", page->links[link].text);
		return a;
	}
	if (wants.form >= 0 && wants.score >= 3) {
		a.step = PORTAL_NEEDS_INPUT;
		a.url = strdup(base_url);
		a.score = wants.score;
		describe(&a, "the form", page->forms[wants.form].text);
		return a;
	}
	if (page->refresh) {
		a.url = portal_resolve(base_url, page->refresh);
		if (a.url) {
			a.step = PORTAL_REDIRECT;
			describe(&a, "the redirect to", a.url);
		}
	}
	return a;
}

void portal_action_clear(struct portal_action *a) {
	for (int i = 0; i < a->npairs; i++) {
		free(a->names[i]);
		free(a->values[i]);
	}
	free(a->names);
	free(a->values);
	free(a->url);
	free(a->what);
	memset(a, 0, sizeof(*a));
}
