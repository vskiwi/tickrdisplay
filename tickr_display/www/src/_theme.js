// Light/dark theme (client-side only, nothing stored on the device).
// Applies localStorage["tickr_theme"] (auto|light|dark) before the first paint
// so there is no light flash; "auto" follows prefers-color-scheme and tracks
// changes at runtime. Must stay inline in <head> of every page.
(function(){var d=document.documentElement,m=matchMedia('(prefers-color-scheme:dark)');function g(){try{return localStorage.tickr_theme}catch(e){}}function a(){var t=g()||'auto';d.dataset.theme=t=='auto'?(m.matches?'dark':'light'):t;var s=document.getElementById('th');if(s)s.value=t}window.setTheme=function(v){try{localStorage.tickr_theme=v}catch(e){}a()};m.onchange=a;a();addEventListener('DOMContentLoaded',a)})()
