/*
 * How tilewin-portal reads login pages of public Wi-Fi and what it does on
 * them: the form and button it sends, the checkboxes it ticks and leaves, the
 * links it follows, the pages it leaves to the person, and where a page sends
 * itself on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../portal/accept.h"
#include "../portal/html.h"

static int failures;

static void check(int ok, const char *what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		failures++;
	}
}

static const char *pair(const struct portal_action *a, const char *name) {
	for (int i = 0; i < a->npairs; i++) {
		if (strcmp(a->names[i], name) == 0) {
			return a->values[i];
		}
	}
	return NULL;
}

static struct portal_action decide(const char *html, const char *base) {
	struct html_page *page = html_parse(html);
	struct portal_action a = portal_decide(page, base);
	html_free(page);
	return a;
}

int main(void) {
	// a hotel: terms to tick, a newsletter to leave, a hidden token, a POST
	struct portal_action a = decide(
		"<html><head><title>Hotel WiFi</title></head><body>"
		"<FORM method=POST action='/login.php'>"
		"<input type=hidden name=token value=\"a&amp;b\">"
		"<label><input type=checkbox name=terms> I accept the <a href=/agb>terms of use</a></label>"
		"<input type=checkbox id=nl name=newsletter value=yes><label for=nl>Send me the newsletter</label>"
		"<input type=text name=q placeholder=Search style='display: none'>"
		"<button type=submit name=action value=go>Connect now</button>"
		"<button type=submit name=action value=no>Decline</button>"
		"</form></body></html>", "http://portal.example/welcome?x=1");
	check(a.step == PORTAL_SUBMIT, "a hotel's form is sent");
	check(a.post && a.url && strcmp(a.url, "http://portal.example/login.php") == 0,
		"by POST to its action, made absolute");
	check(pair(&a, "token") && strcmp(pair(&a, "token"), "a&b") == 0,
		"with the hidden token, entities decoded");
	check(pair(&a, "terms") && strcmp(pair(&a, "terms"), "on") == 0, "the terms ticked");
	check(!pair(&a, "newsletter"), "the newsletter left unticked");
	check(pair(&a, "action") && strcmp(pair(&a, "action"), "go") == 0,
		"and the button Connect pressed, not Decline");
	portal_action_clear(&a);

	// German, an input submit, a GET form, a select and radios
	a = decide("<form action=\"go.cgi\"><select name=lang><option value=en>English"
		"<option value=de selected>Deutsch</select>"
		"<input type=radio name=plan value=free> <input type=radio name=plan value=paid>"
		"<input type=checkbox name=agb value=1 required> Ich habe die AGB gelesen"
		"<input type=submit value=\"Ich stimme zu &amp; weiter\"></form>",
		"http://10.0.0.1/portal/start.html");
	check(a.step == PORTAL_SUBMIT && !a.post, "a German form is sent by GET");
	check(a.url && strcmp(a.url, "http://10.0.0.1/portal/go.cgi") == 0, "to a relative action");
	check(pair(&a, "lang") && strcmp(pair(&a, "lang"), "de") == 0, "the selected option goes");
	check(pair(&a, "plan") && strcmp(pair(&a, "plan"), "free") == 0 && a.npairs == 3,
		"the first radio of a group: lang, plan, agb (3 pairs)");
	portal_action_clear(&a);

	// a page that wants an email: left to the person
	a = decide("<form method=post><input type=email name=guest_email required>"
		"<input type=checkbox name=tos> <button>Accept &amp; connect</button></form>",
		"https://wifi.example/");
	check(a.step == PORTAL_NEEDS_INPUT, "a form that wants an email is left to the person");
	portal_action_clear(&a);

	a = decide("<form><input name=roomNumber placeholder='Room number'>"
		"<input name=lastname placeholder='Last name'><input type=submit value=Login></form>",
		"http://hotel.example/");
	check(a.step == PORTAL_NEEDS_INPUT, "so is one that wants a room number and a name");
	portal_action_clear(&a);

	// a page with only a link onwards
	a = decide("<p>Welcome to free Wi-Fi!</p><a href='/info'>More information</a>"
		"<a href=\"step2.html\" class=btn>Continue to the internet</a>",
		"http://captive.example/a/b.html");
	check(a.step == PORTAL_LINK && a.url && strcmp(a.url, "http://captive.example/a/step2.html") == 0,
		"a link onwards is followed, not More information");
	portal_action_clear(&a);

	// redirects
	a = decide("<meta http-equiv=\"Refresh\" content=\"0; URL='http://login.example/x?a=1'\">",
		"http://neverssl.example/");
	check(a.step == PORTAL_REDIRECT && a.url && strcmp(a.url, "http://login.example/x?a=1") == 0,
		"a meta refresh is followed");
	portal_action_clear(&a);
	a = decide("<html><script>if (1) { window.location.href = \"/splash?mac=aa\"; }</script>",
		"http://192.168.0.1:8080/");
	check(a.step == PORTAL_REDIRECT && a.url && strcmp(a.url, "http://192.168.0.1:8080/splash?mac=aa") == 0,
		"so is a script setting location");
	portal_action_clear(&a);
	a = decide("<script>var a = location.href == 'x';</script>", "http://x.example/");
	check(a.step == PORTAL_NOTHING, "but not a script comparing it");
	portal_action_clear(&a);

	// a search form beside nothing: no way in
	a = decide("<form action=/search><input name=q><input type=submit value=Go></form>",
		"http://example.com/");
	check(a.step == PORTAL_NOTHING, "a search form is not a way in");
	portal_action_clear(&a);

	// buttons outside any form, as a script would send them
	a = decide("<div><input type=checkbox id=t name=accept_terms><label for=t>I agree to the terms</label>"
		"<button id=go>Get online</button></div>", "http://portal.example/");
	check(a.step == PORTAL_SUBMIT && pair(&a, "accept_terms"),
		"fields outside a form are sent as one, the checkbox ticked");
	portal_action_clear(&a);

	// an image button
	a = decide("<form method=post action=/ok><input type=image name=btn src=go.png alt='Accept'></form>",
		"http://portal.example/");
	check(a.step == PORTAL_SUBMIT && pair(&a, "btn.x") && pair(&a, "btn.y"),
		"an image button sends where it was clicked");
	portal_action_clear(&a);

	// scores
	check(portal_text_score("Book now") <= 0, "'ok' is not found in 'book'");
	check(portal_text_score("Akzeptieren und verbinden") >= 10, "German accept");
	check(portal_text_score("Nutzungsbedingungen") < 0, "the terms themselves are no button");
	check(portal_text_score("Accept the terms of use") >= 7, "but accepting them is");
	check(portal_text_score("Ablehnen") < 0, "declining is a way out");
	check(portal_is_marketing("Ja, ich möchte Werbung per E-Mail erhalten"), "marketing in German");
	check(!portal_is_marketing("I accept the terms"), "the terms are no marketing");

	char *u = portal_resolve("http://a.example/x/y?z", "../q");
	check(u && strcmp(u, "http://a.example/q") == 0, "relative URLs resolve");
	free(u);
	u = portal_resolve("http://a.example/", "javascript:void(0)");
	check(!u, "javascript: links are none");
	free(u);

	if (failures) {
		printf("%d failed\n", failures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
