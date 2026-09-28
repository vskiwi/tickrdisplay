// Browser crypto for the pairing wizard (docs/MULTI_DEVICE.md "The pairing protocol"). crypto.subtle is not
// available on http://, so these are own implementations of X25519 (RFC 7748, Montgomery
// ladder over BigInt - not constant-time, acceptable for an ephemeral key in the user's own
// browser), SHA-256 (FIPS 180-4), HMAC (RFC 2104) and HKDF (RFC 5869). No third-party code.
// Verified in node against the RFC 7748 §5.2/§6.1 vectors and the fixed protocol vectors of
// test/test_pairing (same numbers as the firmware).
var TC=(function(){
var P=(1n<<255n)-19n,M=(1n<<255n)-1n;
function le(b){var r=0n;for(var i=b.length-1;i>=0;i--)r=(r<<8n)|BigInt(b[i]);return r}
function tole(n){var b=new Uint8Array(32);for(var i=0;i<32;i++){b[i]=Number(n&255n);n>>=8n}return b}
function mp(b,e){var r=1n;b%=P;while(e>0n){if(e&1n)r=r*b%P;b=b*b%P;e>>=1n}return r}
function x25519(k,u){
k=k.slice();k[0]&=248;k[31]&=127;k[31]|=64;
var kk=le(k),x1=le(u)&M,x2=1n,z2=0n,x3=x1,z3=1n,sw=0n,t,kt,A,AA,B,BB,E,C,D,DA,CB,q;
for(t=254;t>=0;t--){kt=(kk>>BigInt(t))&1n;sw^=kt;if(sw){q=x2;x2=x3;x3=q;q=z2;z2=z3;z3=q}sw=kt;
A=(x2+z2)%P;AA=A*A%P;B=(x2-z2+P)%P;BB=B*B%P;E=(AA-BB+P)%P;C=(x3+z3)%P;D=(x3-z3+P)%P;DA=D*A%P;CB=C*B%P;
x3=(DA+CB)%P;x3=x3*x3%P;z3=(DA-CB+P)%P;z3=x1*(z3*z3%P)%P;x2=AA*BB%P;z2=E*((AA+121665n*E)%P)%P}
if(sw){q=x2;x2=x3;x3=q;q=z2;z2=z3;z3=q}
return tole(x2*mp(z2,P-2n)%P)}
function pub(sk){var b=new Uint8Array(32);b[0]=9;return x25519(sk,b)}
// Round constants / initial hash: fractional parts of the cube resp. square roots of the first 64 primes.
var K=[],H0=[],pr=[],n=2;while(pr.length<64){for(var i=0;i<pr.length&&n%pr[i];i++);if(i==pr.length)pr.push(n);n++}
for(n=0;n<64;n++){K[n]=Math.pow(pr[n],1/3)%1*4294967296|0;if(n<8)H0[n]=Math.sqrt(pr[n])%1*4294967296|0}
function rr(x,n){return x>>>n|x<<32-n}
function sha(m){var l=m.length,n=(l+9+63>>6)<<6,q=new Uint8Array(n),v=new DataView(q.buffer),H=H0.slice(),w=new Int32Array(64),i,j,a,b,c,d,e,f,g,h,s0,s1,t1,t2;
q.set(m);q[l]=128;v.setUint32(n-4,l<<3>>>0);v.setUint32(n-8,l/0x20000000>>>0);
for(i=0;i<n;i+=64){for(j=0;j<16;j++)w[j]=v.getInt32(i+4*j);
for(;j<64;j++){s0=w[j-15];s1=w[j-2];w[j]=w[j-16]+(rr(s0,7)^rr(s0,18)^s0>>>3)+w[j-7]+(rr(s1,17)^rr(s1,19)^s1>>>10)|0}
a=H[0];b=H[1];c=H[2];d=H[3];e=H[4];f=H[5];g=H[6];h=H[7];
for(j=0;j<64;j++){t1=h+(rr(e,6)^rr(e,11)^rr(e,25))+(e&f^~e&g)+K[j]+w[j]|0;t2=(rr(a,2)^rr(a,13)^rr(a,22))+(a&b^a&c^b&c)|0;h=g;g=f;f=e;e=d+t1|0;d=c;c=b;b=a;a=t1+t2|0}
H[0]=H[0]+a|0;H[1]=H[1]+b|0;H[2]=H[2]+c|0;H[3]=H[3]+d|0;H[4]=H[4]+e|0;H[5]=H[5]+f|0;H[6]=H[6]+g|0;H[7]=H[7]+h|0}
var o=new Uint8Array(32),ov=new DataView(o.buffer);for(i=0;i<8;i++)ov.setInt32(4*i,H[i]);return o}
function cat(){var n=0,i,o,p=0;for(i=0;i<arguments.length;i++)n+=arguments[i].length;o=new Uint8Array(n);for(i=0;i<arguments.length;i++){o.set(arguments[i],p);p+=arguments[i].length}return o}
function hmac(k,m){if(k.length>64)k=sha(k);var ip=new Uint8Array(64),op=new Uint8Array(64),i;for(i=0;i<64;i++){ip[i]=(k[i]||0)^54;op[i]=(k[i]||0)^92}return sha(cat(op,sha(cat(ip,m))))}
function hkdf(prk,info,L){var o=new Uint8Array(L),t=new Uint8Array(0),c=1,p=0;while(p<L){t=hmac(prk,cat(t,info,new Uint8Array([c++])));o.set(t.subarray(0,Math.min(32,L-p)),p);p+=32}return o}
function str(s){return new TextEncoder().encode(s)}
function rnd(n){var b=new Uint8Array(n);crypto.getRandomValues(b);return b}
function b64(b){return btoa(String.fromCharCode.apply(null,b))}
function unb64(s){var t=atob(s),b=new Uint8Array(t.length),i;for(i=0;i<t.length;i++)b[i]=t.charCodeAt(i);return b}
function hex(b){var s='',i;for(i=0;i<b.length;i++)s+=(b[i]<16?'0':'')+b[i].toString(16);return s}
function eq(a,b){var d=a.length^b.length,i;for(i=0;i<a.length&&i<b.length;i++)d|=a[i]^b[i];return d===0}
var CH="ACDEFGHJKMNPRTWX";
function chkw(b){return CH[b[0]>>4]+CH[b[0]&15]+CH[b[1]>>4]+CH[b[1]&15]}
// Session keys (docs/MULTI_DEVICE.md "The pairing protocol"): PRK = HKDF-Extract(n_d, K); K_c, K_e bound to both public keys; check word.
function derive(K,n,pki,pkd){var prk=hmac(n,K);return{kc:hkdf(prk,cat(str("tickr-pair-v1 confirm"),pki,pkd),32),ke:hkdf(prk,cat(str("tickr-pair-v1 box"),pki,pkd),32),chk:chkw(hkdf(prk,str("tickr-pair-v1 check"),2))}}
function groupKey(K,n,pki,pka){return hkdf(hmac(n,K),cat(str("tickr-group-v1 box"),pki,pka),32)}
// Enc()/Dec(): HKDF keystream XOR + 16-byte HMAC tag.
function seal(ke,m){var ks=hkdf(ke,str("box-ks"),m.length),o=new Uint8Array(m.length+16),i;for(i=0;i<m.length;i++)o[i]=m[i]^ks[i];o.set(hmac(ke,cat(str("box-tag"),o.subarray(0,m.length))).subarray(0,16),m.length);return o}
function open(ke,box){var n=box.length-16,ct=box.subarray(0,n),i,ks,m;if(n<1||!eq(hmac(ke,cat(str("box-tag"),ct)).subarray(0,16),box.subarray(n)))return null;ks=hkdf(ke,str("box-ks"),n);m=new Uint8Array(n);for(i=0;i<n;i++)m[i]=ct[i]^ks[i];return m}
// Credentials wire layout (74 B): id(8) || secret(32) || epoch BE(2) || name (31, NUL padded).
function credsUnpack(p){var name='',i;if(p.length!=74)return null;for(i=42;i<74&&p[i];i++)name+=String.fromCharCode(p[i]);return{id:p.slice(0,8),secret:p.slice(8,40),epoch:p[40]<<8|p[41],name:name}}
function credsPack(c){var p=new Uint8Array(74),i;p.set(c.id,0);p.set(c.secret,8);p[40]=c.epoch>>8;p[41]=c.epoch&255;for(i=0;i<c.name.length&&i<31;i++)p[42+i]=c.name.charCodeAt(i);return p}
return{x25519:x25519,pub:pub,sha:sha,hmac:hmac,hkdf:hkdf,cat:cat,str:str,rnd:rnd,b64:b64,unb64:unb64,hex:hex,eq:eq,derive:derive,groupKey:groupKey,seal:seal,open:open,credsUnpack:credsUnpack,credsPack:credsPack}
})();
