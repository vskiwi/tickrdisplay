#pragma once
//
// Web authentication policy (pure, host-tested) - docs/WEB_UI.md "API token in the browser".
//
// A 401 carries "WWW-Authenticate: Basic" only for a *navigation* - a request
// whose Accept header asks for HTML (address bar, link, plain <form>): there the
// browser's Basic prompt is the only way to attach the token (partition
// downloads on /dev, a stock-style form post to /update). fetch()/XMLHttpRequest
// send "Accept: */*" and get a bare 401, so the pages never trigger the prompt;
// they show their inline token field instead. curl sends "*/*" too, but -u sends
// the Basic credentials preemptively and never needs the challenge.
//
#include <string.h>

// True when the Accept header of a request asks for text/html.
static inline bool auth_challenge_wanted(const char* accept) {
    return accept != nullptr && strstr(accept, "text/html") != nullptr;
}
