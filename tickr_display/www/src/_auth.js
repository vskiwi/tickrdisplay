// Shared by /, /system and /dev (docs/WEB_UI.md "API token in the browser"): the API token lives in
// localStorage.tickr_token and travels as X-Api-Token; the pages never rely on the browser's
// Basic prompt. Read-only mode (RO): body.ro greys every control of class "w" and shows the
// token bar (_tokbar.html). Entered when the device has a token and this browser has none
// (status.auth_enabled && !T) or when a request answered 401 (on401 - shown once, no retry
// loop). tokSave() stores the token and reloads the page, which re-initialises everything.
var T=null,RO=false;try{T=localStorage.tickr_token||null}catch(e){}
function hdr(h){h=h||{};if(T)h['X-Api-Token']=T;return h}
function ro(on,m){RO=!!on;document.body.classList.toggle('ro',RO);var b=$('tokbar');if(b){b.hidden=!RO;$('tokmsg').textContent=m||''}}
function on401(){if(!RO)ro(true,T?'This device does not accept the stored token.':'')}
function tokSave(v){v=(v||'').trim();T=v||null;try{if(T)localStorage.tickr_token=T;else delete localStorage.tickr_token}catch(e){}location.reload()}
