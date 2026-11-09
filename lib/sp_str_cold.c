/* sp_str_cold.c -- the cold half of the String helpers in lib/sp_str.c.
 *
 * Case mapping, strip / chomp / chop / succ, delete_prefix / delete_suffix,
 * sub / gsub, inspect / dump / undump and the Symbol name helpers. They are
 * long, called once per String and never inline into anything, so they sit
 * in a unit of their own: with them in lib/sp_str.c, the budget gcc gives a
 * unit for inlining (--param inline-unit-growth, a share of the unit's size)
 * ran out before the concat, plus and allocation helpers, and
 * sp_str_is_binary, sp_str_alloc and sp_str_byte_len became calls there.
 * The declarations are in sp_str.h as before; sp_bytestr is the one helper
 * both files use. */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include "sp_str.h"
extern SP_TLS int sp_re_sub_matched;   /* sp_re.h: the bang forms' "a substitution happened" */
/* sp_re.h: what gsub / sub / scan with a String pattern leave in `$~`, when
   the program reads it (sp_re_track_last) */
extern int sp_re_track_last;
void sp_re_clear_last_match(void);
void sp_re_set_lit_match(const char *str, sp_int beg, sp_int end);

/* general categories, for what inspect escapes; `spin ext build` vendors
   the runtime flat, lib/regexp/ beside lib/ */
#if defined(__has_include)
#  if __has_include("regexp/re_prop.h")
#    include "regexp/re_prop.h"
#  else
#    include "re_prop.h"
#  endif
#else
#  include "regexp/re_prop.h"
#endif

/* Does String#inspect spell codepoint cp as \uXXXX? CRuby escapes what
   rb_enc_isprint refuses: a C1 control (Cc), a line or paragraph separator
   (Zl, Zp), a surrogate (Cs) and an unassigned codepoint (Cn). A format
   character (Cf) and a space (Zs) print as they are. */
static int sp_cp_inspect_escapes(uint32_t cp) {
  size_t lo = 0, hi = RE_PROP_GC_RUN_COUNT;
  while (hi - lo > 1) {
    size_t mid = (lo + hi) / 2;
    if ((re_prop_gc_runs[mid] >> RE_PROP_GC_BITS) <= cp) lo = mid;
    else hi = mid;
  }
  const char *gc = re_prop_gc_names[re_prop_gc_runs[lo] & ((1u << RE_PROP_GC_BITS) - 1)];
  return !strcmp(gc, "Cc") || !strcmp(gc, "Zl") || !strcmp(gc, "Zp") ||
         !strcmp(gc, "Cs") || !strcmp(gc, "Cn");
}

/* Hex-digit value, used by sp_str_undump's \xNN / \uNNNN unescape. */
static int _sp_hexval(unsigned char d){return (d<='9')?(d-'0'):(tolower(d)-'a'+10);}

/* gsub/sub replacement backslash expansion (defined near sp_str_gsub). */
static char *sp_str_rep_expand(const char *rep, const char *match, size_t mlen, size_t *olen);
/* String#inspect: wrap in double quotes and escape \, ", \n, \t, \r,
   plus any non-printable byte as \xNN. Output is always ASCII-safe. */
const char*sp_str_inspect(const char*s){SP_GC_ROOT_STR(s);if(!s){char*r=sp_str_alloc_raw(4);r[0]='n';r[1]='i';r[2]='l';r[3]=0;return r;}size_t sl=sp_str_byte_len(s);size_t cap=(sl*6)+3;char*r=sp_str_alloc_raw(cap);size_t o=0;r[o++]='"';for(size_t i=0;i<sl;i++){unsigned char c=(unsigned char)s[i];if(c=='\\'||c=='"'){r[o++]='\\';r[o++]=c;}
/* `#` is escaped only where it would start an interpolation (#3558) */
else if(c=='#'&&i+1<sl&&(s[i+1]=='{'||s[i+1]=='$'||s[i+1]=='@')){r[o++]='\\';r[o++]='#';}
else if(c=='\a'){r[o++]='\\';r[o++]='a';}
else if(c=='\b'){r[o++]='\\';r[o++]='b';}
else if(c=='\t'){r[o++]='\\';r[o++]='t';}
else if(c=='\n'){r[o++]='\\';r[o++]='n';}
else if(c=='\v'){r[o++]='\\';r[o++]='v';}
else if(c=='\f'){r[o++]='\\';r[o++]='f';}
else if(c=='\r'){r[o++]='\\';r[o++]='r';}
else if(c==0x1b){r[o++]='\\';r[o++]='e';}
else if(c<0x20||c==0x7f){/* other control bytes render as \uNNNN (UTF-8 default, matching CRuby source-literal strings); a BINARY string -- what pack and Random#bytes answer -- renders \xNN as CRuby does for ASCII-8BIT (#3553) */if(sp_str_is_binary(s)){snprintf(r+o,5,"\\x%02X",c);o+=4;}else{snprintf(r+o,7,"\\u%04X",c);o+=6;}}
/* A byte that is not part of a valid UTF-8 sequence escapes as \xNN, the way
   CRuby renders it: `"a\x80b".inspect` is "a\x80b". Passing the raw byte
   through made the inspect output itself invalid UTF-8, so a terminal drew
   U+FFFD -- inspect lost the one thing it was being asked about. A BINARY
   string escapes every high byte, as the control-byte arm above already does
   for it. A valid sequence is copied through unchanged. */
else if(c>=0x80){
  int extra=(c&0xE0)==0xC0?1:(c&0xF0)==0xE0?2:(c&0xF8)==0xF0?3:-1;
  int ok=extra>0&&!sp_str_is_binary(s)&&i+(size_t)extra<sl;
  for(int k=1;ok&&k<=extra;k++)if(((unsigned char)s[i+(size_t)k]&0xC0)!=0x80)ok=0;
  if(!ok){snprintf(r+o,5,"\\x%02X",c);o+=4;}
  else{
    /* a valid character that does not print is spelled by its codepoint,
       as CRuby does: "\u0080", "\u2028", "\u{E0080}" */
    uint32_t cp=(uint32_t)(c&(extra==1?0x1F:extra==2?0x0F:0x07));
    for(int k=1;k<=extra;k++)cp=(cp<<6)|((unsigned char)s[i+(size_t)k]&0x3F);
    if(sp_cp_inspect_escapes(cp)){
      if(cp>0xFFFF){int w=snprintf(r+o,12,"\\u{%X}",(unsigned)cp);o+=(size_t)w;}
      else{snprintf(r+o,7,"\\u%04X",(unsigned)cp);o+=6;}
    }
    else for(int k=0;k<=extra;k++)r[o++]=s[i+(size_t)k];
    i+=(size_t)extra;}
}
else{r[o++]=(char)c;}}r[o++]='"';r[o]=0;sp_str_set_len(r,o);return r;}
/* A symbol prints without quotes when its name is a plain identifier (an
   @ivar / @@cvar / $gvar, or a bare name optionally ending in ? ! =) or a
   known operator method name; otherwise it is quoted like a string: :"a b". */
sp_bool sp_sym_plain_name_p(const char *p, sp_bool allow_suffix) {
  /* Identifier classification, locale-independent on purpose (no LC_CTYPE):
     ASCII letters/digits/underscore by table, and any byte >= 0x80 counts as
     an identifier character -- CRuby treats non-ASCII codepoints as valid
     unquoted symbol characters (`:café` inspects without quotes). */
  unsigned char c = (unsigned char)*p;
  if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80)) return FALSE;
  for (p++; (c = (unsigned char)*p) != '\0'; p++)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c >= 0x80)) break;
  if (allow_suffix && (*p == '?' || *p == '!' || *p == '=')) p++;
  return *p == '\0';
}
sp_bool sp_sym_simple_p(const char *n) {SP_GC_ROOT_STR(n);
  if (!n || !*n) return FALSE;
  /* A name holding a NUL is never a plain identifier, and the walks below all
     stop at it -- `:"a\0b"` would read as the plain name "a" and print bare
     (#nul symbols). Quote it, and sp_str_inspect spells the byte. */
  if (sp_str_byte_len(n) != strlen(n)) return FALSE;
  if (*n == '$') return sp_sym_plain_name_p(n + 1, FALSE);
  if (*n == '@') { const char *p = n + 1; if (*p == '@') p++; return sp_sym_plain_name_p(p, FALSE); }
  if (sp_sym_plain_name_p(n, TRUE)) return TRUE;
  static const char *const ops[] = {
    "+", "-", "*", "/", "%", "**", "==", "===", "!=", "=~", "!~",
    "<", "<=", ">", ">=", "<=>", "<<", ">>", "&", "|", "^", "~",
    "!", "+@", "-@", "[]", "[]=", "`", NULL };
  for (int i = 0; ops[i]; i++) if (!strcmp(n, ops[i])) return TRUE;
  return FALSE;
}
const char *sp_sym_inspect_name(const char *name) {SP_GC_ROOT_STR(name);
  /* Build ":" + body directly rather than sp_str_concat(":", body): a bare
     literal like ":" has no length-marker byte, so sp_str_byte_len would read
     one byte before it (out of bounds of the .rodata constant). `body` is a
     real spinel string (the symbol's name, or its inspected form), so measuring
     it is safe. */
  const char *body = sp_sym_simple_p(name) ? name : sp_str_inspect(name);
  SP_GC_ROOT_STR(body);   /* the non-simple branch just allocated body; keep it live across sp_str_alloc's GC */
  size_t bl = sp_str_byte_len(body);
  char *r = sp_str_alloc(1 + bl);
  r[0] = ':';
  memcpy(r + 1, body, bl);
  return r;
}
/* A symbol hash key in the `key: value` short form: a name that can be
   written as a label is bare -- an identifier, with an optional trailing `?`
   or `!` -- and any other is string-quoted (`"k space": ...`, `"+": ...`,
   `"a=": ...`, `"@a": ...`), as CRuby's Hash#inspect writes it; no leading
   colon. */
const char *sp_sym_inspect_key(const char *name) {SP_GC_ROOT_STR(name);
  size_t l = name ? sp_str_byte_len(name) : 0;
  int label = l > 0 && l == strlen(name) && name[l - 1] != '=' && sp_sym_plain_name_p(name, TRUE);
  return label ? name : sp_str_inspect(name);
}
/* Map every codepoint of s through fn; ß (U+00DF) upcases to "SS" when
   up is set, so allocate room for a 3x expansion. */
/* byte_len, not strlen: an embedded NUL is a byte of the string, and mapping
   only up to it silently truncated the result (the opportunistic NUL policy --
   fix what is met). U+0000 maps to itself and encodes as the one byte. */
static const char*sp_str_case_bin(const char*s,int mode){size_t l=sp_str_byte_len(s);char*r=sp_str_alloc_raw(l+1);for(size_t i=0;i<l;i++){unsigned char c=(unsigned char)s[i];int lo=c>=0x61&&c<=0x7A,up=c>=0x41&&c<=0x5A;int toup=mode==0||(mode==3&&i==0);int todn=mode==1||(mode==3&&i>0);if(mode==2)c=lo?c-32:up?c+32:c;else if(toup&&lo)c-=32;else if(todn&&up)c+=32;r[i]=(char)c;}r[l]=0;sp_str_set_len(r,l);sp_str_mark_binary(r);return r;}
static const char*sp_str_case_map(const char*s,uint32_t(*fn)(uint32_t),int up){SP_GC_ROOT_STR(s);if(sp_str_is_binary(s))return sp_str_case_bin(s,up?0:1);
  size_t l=sp_str_byte_len(s);char*r=sp_str_alloc_raw(l*3+1);size_t oi=0;
  for(size_t i=0;i<l;){uint32_t cp;int n=sp_utf8_decode(s+i,&cp);i+=(size_t)n;
    uint32_t m=fn(cp);
    /* a mapping to more than one codepoint ("ß".upcase is "SS") */
    if(m==cp){uint32_t to[3];int k=sp_uc_special_of(up?SP_UC_UPPER:SP_UC_LOWER,cp,to);
      if(k){for(int j=0;j<k;j++)oi+=(size_t)sp_utf8_encode(to[j],r+oi);continue;}}
    oi+=(size_t)sp_utf8_encode(m,r+oi);}
  r[oi]=0;sp_str_set_len(r,oi);return r;
}
/* casecmp? compares the Unicode case FOLDING of both sides, not bytes under
   ASCII tolower: "\u00c9".casecmp?("\u00e9") is true. Folding here is the
   lowercase mapping above over the same ranges, plus the few sources in them
   whose folding is not their lowercase: sharp s (and capital sharp s) fold to
   "ss", long s to "s", micro sign to Greek mu, and dotted capital I to "i"
   and a combining dot. */
static size_t sp_uc_fold_into(uint32_t cp,char*o){return sp_uc_fold_utf8(cp,o);}
static char*sp_str_fold_buf(const char*s,size_t*out){
  size_t l=sp_str_byte_len(s);char*r=(char*)malloc(l*3+1);if(!r){perror("malloc");exit(1);}size_t oi=0;
  for(size_t i=0;i<l;){uint32_t cp;int n=sp_utf8_decode(s+i,&cp);i+=(size_t)n;
    /* a byte that starts no valid UTF-8 character (a BINARY Symbol's
       name, "\xC3".b.to_sym) is no character to fold: kept as it is */
    if(n==1&&cp>=0x80){r[oi++]=(char)cp;continue;}
    oi+=sp_uc_fold_into(cp,r+oi);}
  *out=oi;return r;
}
sp_bool sp_str_casecmp_p(const char*a,const char*b){if(!a)sp_nil_recv("casecmp?");if(!b)return FALSE;
  size_t la=sp_str_byte_len(a),lb=sp_str_byte_len(b);int ascii=1;
  for(size_t i=0;i<la&&ascii;i++)if((unsigned char)a[i]>=0x80)ascii=0;
  for(size_t i=0;i<lb&&ascii;i++)if((unsigned char)b[i]>=0x80)ascii=0;
  /* a BINARY String folds A-Z only: its high bytes are no characters */
  if(ascii||sp_str_is_binary(a)||sp_str_is_binary(b))return sp_str_casecmp(a,b)==0;
  size_t fa,fb;char*x=sp_str_fold_buf(a,&fa);char*y=sp_str_fold_buf(b,&fb);
  sp_bool eq=fa==fb&&memcmp(x,y,fa)==0;free(x);free(y);return eq;
}
const char*sp_str_upcase(const char*s){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("upcase");return sp_str_case_map(s,sp_uc_toupper,1);}
const char*sp_str_downcase(const char*s){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("downcase");return sp_str_case_map(s,sp_uc_tolower,0);}
static const char*sp_str_swapcase_f(const char*s,uint32_t(*fu)(uint32_t),uint32_t(*fl)(uint32_t)){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("swapcase");if(sp_str_is_binary(s))return sp_str_case_bin(s,2);size_t l=sp_str_byte_len(s);char*r=sp_str_alloc_raw(l*3+1);size_t oi=0;for(size_t i=0;i<l;){uint32_t cp;int n=sp_utf8_decode(s+i,&cp);i+=(size_t)n;
    uint32_t up=fu(cp),lo=fl(cp);uint32_t to[3];int k;
    /* where CRuby's swapcase is neither ("ǅ" swaps to "dž") */
    if(fu==sp_uc_toupper&&(k=sp_uc_special_of(SP_UC_SWAP,cp,to))){for(int j=0;j<k;j++)oi+=(size_t)sp_utf8_encode(to[j],r+oi);}
    else if(up!=cp){/* cp is lowercase -> uppercase */
oi+=(size_t)sp_utf8_encode(up,r+oi);}
else if(lo!=cp){/* cp is uppercase -> lowercase */oi+=(size_t)sp_utf8_encode(lo,r+oi);}
/* a mapping to more than one codepoint: "ß" upcases to "SS", "İ" downcases to "i̇" */
else if((k=sp_uc_special_of(SP_UC_UPPER,cp,to))||(k=sp_uc_special_of(SP_UC_LOWER,cp,to))){for(int j=0;j<k;j++)oi+=(size_t)sp_utf8_encode(to[j],r+oi);}
else oi+=(size_t)sp_utf8_encode(cp,r+oi);}r[oi]=0;sp_str_set_len(r,oi);return r;}
const char*sp_str_swapcase(const char*s){return sp_str_swapcase_f(s,sp_uc_toupper,sp_uc_tolower);}
/* downcase(:fold): the full Unicode case folding ("ß" folds to "ss"); a
   BINARY String folds A-Z only (#11345) */
const char*sp_str_downcase_fold(const char*s){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("downcase");if(sp_str_is_binary(s))return sp_str_case_bin(s,1);
  size_t l=sp_str_byte_len(s);char*r=sp_str_alloc_raw(l*3+1);size_t oi=0;
  for(size_t i=0;i<l;){uint32_t cp;int n=sp_utf8_decode(s+i,&cp);i+=(size_t)n;
    if(n==1&&cp>=0x80){r[oi++]=(char)cp;continue;}
    oi+=sp_uc_fold_utf8(cp,r+oi);}
  r[oi]=0;sp_str_set_len(r,oi);return r;}
/* :fold is for downcasing only (CRuby's ArgumentError) */
SP_NORETURN static void sp_str_fold_not_down(void){sp_raise_cls("ArgumentError","option :fold only allowed for downcasing");}
const char*sp_str_upcase_fold(const char*s){(void)s;sp_str_fold_not_down();}
const char*sp_str_capitalize_fold(const char*s){(void)s;sp_str_fold_not_down();}
const char*sp_str_swapcase_fold(const char*s){(void)s;sp_str_fold_not_down();}
/* The `:ascii` option (upcase(:ascii)) restricts folding to A-Z/a-z and leaves
   every non-ASCII byte untouched, so these copy byte-for-byte. */
const char*sp_str_upcase_ascii(const char*s){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("upcase");size_t l=sp_str_byte_len(s);char*r=sp_str_alloc_raw(l+1);for(size_t i=0;i<l;i++){unsigned char ch=(unsigned char)s[i];r[i]=(ch>='a'&&ch<='z')?(char)(ch-32):(char)ch;}r[l]=0;sp_str_set_len(r,l);return sp_str_bin_like(s,r);}
const char*sp_str_downcase_ascii(const char*s){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("downcase");size_t l=sp_str_byte_len(s);char*r=sp_str_alloc_raw(l+1);for(size_t i=0;i<l;i++){unsigned char ch=(unsigned char)s[i];r[i]=(ch>='A'&&ch<='Z')?(char)(ch+32):(char)ch;}r[l]=0;sp_str_set_len(r,l);return sp_str_bin_like(s,r);}
const char*sp_str_swapcase_ascii(const char*s){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("swapcase");size_t l=sp_str_byte_len(s);char*r=sp_str_alloc_raw(l+1);for(size_t i=0;i<l;i++){unsigned char ch=(unsigned char)s[i];if(ch>='a'&&ch<='z')r[i]=(char)(ch-32);else if(ch>='A'&&ch<='Z')r[i]=(char)(ch+32);else r[i]=(char)ch;}r[l]=0;sp_str_set_len(r,l);return sp_str_bin_like(s,r);}
const char*sp_str_capitalize_ascii(const char*s){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("capitalize");size_t l=sp_str_byte_len(s);char*r=sp_str_alloc_raw(l+1);for(size_t i=0;i<l;i++){unsigned char ch=(unsigned char)s[i];if(i==0)r[i]=(ch>='a'&&ch<='z')?(char)(ch-32):(char)ch;else r[i]=(ch>='A'&&ch<='Z')?(char)(ch+32):(char)ch;}r[l]=0;sp_str_set_len(r,l);return sp_str_bin_like(s,r);}
/* String#dump: a double-quoted, escaped form that sp_str_undump reverses.
   UTF-8 high bytes pass through literally (undump copies them back), so a
   dump/undump round-trip is byte-identical. */
const char*sp_str_dump(const char*s){SP_GC_ROOT_STR(s);
  if(!s)sp_nil_recv("dump");
  size_t n=sp_str_byte_len(s);   /* a NUL is a byte to escape, not the end */
  char*out=sp_str_alloc_raw((n*4)+3);size_t oi=0;
  out[oi++]='"';
  for(size_t i=0;i<n;i++){
    unsigned char c=(unsigned char)s[i];
    if(c=='"'){out[oi++]='\\';out[oi++]='"';}
    else if(c=='\\'){out[oi++]='\\';out[oi++]='\\';}
    /* `#` is escaped only where it would start an interpolation (#3558) */
    else if(c=='#'&&i+1<n&&(s[i+1]=='{'||s[i+1]=='$'||s[i+1]=='@')){out[oi++]='\\';out[oi++]='#';}
    else if(c=='\n'){out[oi++]='\\';out[oi++]='n';}
    else if(c=='\t'){out[oi++]='\\';out[oi++]='t';}
    else if(c=='\r'){out[oi++]='\\';out[oi++]='r';}
    else if(c=='\f'){out[oi++]='\\';out[oi++]='f';}
    else if(c=='\v'){out[oi++]='\\';out[oi++]='v';}
    else if(c=='\a'){out[oi++]='\\';out[oi++]='a';}
    else if(c=='\b'){out[oi++]='\\';out[oi++]='b';}
    else if(c==27){out[oi++]='\\';out[oi++]='e';}
    /* CRuby dumps a NUL as \x00, never \0: the short form would run into a
       following digit (`"\0" "1"` dumps as "\x001", not "\01") */
    else if(c==0){out[oi++]='\\';out[oi++]='x';out[oi++]='0';out[oi++]='0';}
    else if(c<0x20){oi+=(size_t)sprintf(out+oi,"\\x%02X",c);}
    /* #dump promises a pure-ASCII, re-evaluable form: a non-ASCII character
       is written as its \uXXXX escape (#3558) */
    else if(c>=0x80){
      unsigned cp=0;int extra=0;
      if((c&0xE0)==0xC0){cp=c&0x1Fu;extra=1;}
      else if((c&0xF0)==0xE0){cp=c&0x0Fu;extra=2;}
      else if((c&0xF8)==0xF0){cp=c&0x07u;extra=3;}
      else{oi+=(size_t)sprintf(out+oi,"\\x%02X",c);continue;}
      if(i+(size_t)extra>=n){oi+=(size_t)sprintf(out+oi,"\\x%02X",c);continue;}
      for(int k=0;k<extra;k++)cp=(cp<<6)|((unsigned char)s[++i]&0x3Fu);
      if(cp>0xFFFFu)oi+=(size_t)sprintf(out+oi,"\\u{%X}",cp);
      else oi+=(size_t)sprintf(out+oi,"\\u%04X",cp);
    }
    else{out[oi++]=(char)c;}
  }
  out[oi++]='"';out[oi]=0;sp_str_set_len(out,oi);return out;
}
const char*sp_str_delete_prefix(const char*s,const char*p){SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(p);if(!s)sp_nil_recv("delete_prefix");if(!p)return sp_str_one_answer(s);size_t sl=sp_str_byte_len(s),pl=sp_str_byte_len(p);if(pl<=sl&&memcmp(s,p,pl)==0){char*r=sp_str_alloc_raw(sl-pl+1);memcpy(r,s+pl,sl-pl+1);sp_str_set_len(r,sl-pl);return sp_str_bin_like(s,r);}char*r=sp_str_alloc_raw(sl+1);memcpy(r,s,sl+1);sp_str_set_len(r,sl);return sp_str_bin_like(s,r);}
const char*sp_str_delete_suffix(const char*s,const char*p){SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(p);if(!s)sp_nil_recv("delete_suffix");if(!p)return sp_str_one_answer(s);size_t sl=sp_str_byte_len(s),pl=sp_str_byte_len(p);if(pl<=sl&&memcmp(s+sl-pl,p,pl)==0){char*r=sp_str_alloc_raw(sl-pl+1);memcpy(r,s,sl-pl);r[sl-pl]=0;sp_str_set_len(r,sl-pl);return sp_str_bin_like(s,r);}char*r=sp_str_alloc_raw(sl+1);memcpy(r,s,sl+1);sp_str_set_len(r,sl);return sp_str_bin_like(s,r);}
/* strip / lstrip / rstrip. CRuby strips the set "\0\t\n\v\f\r " from the
   ends -- i.e. isspace() plus the NUL byte. Use sp_str_byte_len (not
   strlen) so a heap string carrying an embedded NUL (e.g. from pack /
   concat) is measured and stripped correctly; the result is a
   length-tracked heap string so any interior NUL survives. (A frozen
   literal with an embedded NUL is still truncated at the C level -- that
   needs length-tracked literals, out of scope.) */
const char*sp_str_strip(const char*s){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("strip");size_t len=sp_str_byte_len(s);size_t a=0;while(a<len&&(isspace((unsigned char)s[a])||s[a]=='\0'))a++;size_t b=len;while(b>a&&(isspace((unsigned char)s[b-1])||s[b-1]=='\0'))b--;size_t n=b-a;char*r=sp_str_alloc(n);memcpy(r,s+a,n);r[n]=0;return sp_str_bin_like(s,r);}
const char*sp_str_chomp(const char*s){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("chomp");size_t l=sp_str_byte_len(s);if(l>=2&&s[l-2]=='\r'&&s[l-1]=='\n')l-=2;else if(l>0&&s[l-1]=='\n')l--;else if(l>0&&s[l-1]=='\r')l--;char*r=sp_str_alloc_raw(l+1);memcpy(r,s,l);r[l]=0;sp_str_set_len(r,l);return sp_str_bin_like(s,r);}
/* Issue #881: `"hello!".chomp("!")` strips the explicit separator.
   Empty sep strips any trailing newlines (CRuby paragraph mode).
   NULL sep is caller's responsibility (codegen routes nil to a
   no-op before calling). */
const char *sp_str_chomp_sep(const char *s, const char *sep) {SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(sep);
  if (!s) sp_nil_recv("chomp");
  size_t l = sp_str_byte_len(s);   /* byte-exact (#4527) */
  /* "\n" is the record separator's own case: it also takes a trailing "\r\n" or "\r" */
  if (sep && sep[0] == '\n' && sp_str_byte_len(sep) == 1) return sp_str_chomp(s);
  if (!sep || !sp_str_byte_len(sep)) {
    /* Empty sep = paragraph mode: strip trailing \r\n pairs and
       standalone \n's, but NOT standalone \r's. A trailing \r that
       is not part of a \r\n pair stops the stripping. */
    while (l > 0) {
      if (l >= 2 && s[l-2] == '\r' && s[l-1] == '\n') { l -= 2; continue; }
      if (s[l-1] == '\n') { l--; continue; }
      break;
    }
  }
else {
    size_t sl = sp_str_byte_len(sep);
    if (sl <= l && memcmp(s + l - sl, sep, sl) == 0) l -= sl;
  }
  char *r = sp_str_alloc_raw(l + 1);
  memcpy(r, s, l);
  r[l] = 0;
  sp_str_set_len(r, l);
  return sp_str_bin_like(s,r);
}
const char*sp_str_chop(const char*s){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("chop");size_t l=sp_str_byte_len(s);if(l>0){if(l>=2&&s[l-2]=='\r'&&s[l-1]=='\n')l-=2;else{l--;/* back up over any UTF-8 continuation bytes to the char boundary (#3085) */while(l>0&&((unsigned char)s[l]&0xC0)==0x80)l--;}}char*r=sp_str_alloc_raw(l+1);memcpy(r,s,l);r[l]=0;sp_str_set_len(r,l);return sp_str_bin_like(s,r);}
/* byte-exact search and lengths: strstr/strlen stop at an embedded NUL, so a
   pattern or subject holding one matched the wrong place or not at all (the
   opportunistic NUL policy -- fix what is met). */
/* slice!(str): s without the first occurrence of pat, or s itself when there
   is none. Unlike sub it sets no `$~`, as CRuby's slice! sets none. */
const char*sp_str_remove_first(const char*s,const char*pat){SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(pat);if(!s||!pat)return sp_str_one_answer(s);size_t pl=sp_str_byte_len(pat),sl=sp_str_byte_len(s);const char*f=sp_bytestr(s,sl,pat,pl);if(!f)return sp_str_one_answer(s);size_t n=(size_t)(f-s);char*r=sp_str_alloc_raw(sl-pl+1);memcpy(r,s,n);memcpy(r+n,f+pl,sl-n-pl);r[sl-pl]=0;sp_str_set_len(r,sl-pl);return r;}
const char*sp_str_sub(const char*s,const char*pat,const char*rep){SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(pat);SP_GC_ROOT_STR(rep);if(!s)sp_nil_recv("sub");if(!pat||!rep)return sp_str_one_answer(s);size_t pl0=sp_str_byte_len(pat),sl0=sp_str_byte_len(s);const char*f=sp_bytestr(s,sl0,pat,pl0);if(!f){if(sp_re_track_last)sp_re_clear_last_match();return sp_str_dup(s);}sp_re_sub_matched=1;if(sp_re_track_last)sp_re_set_lit_match(s,(sp_int)(f-s),(sp_int)(f-s+pl0));size_t el=0;char*rep_exp=sp_str_rep_expand(rep,pat,pl0,&el);const char*rp=rep_exp?rep_exp:rep;size_t pl=pl0,rl=rep_exp?el:sp_str_byte_len(rp),sl=sl0;char*r=sp_str_alloc_raw(sl-pl+rl+1);size_t n=f-s;memcpy(r,s,n);memcpy(r+n,rp,rl);memcpy(r+n+rl,f+pl,sl-n-pl);r[sl-pl+rl]=0;sp_str_set_len(r,sl-pl+rl);if(rep_exp)free(rep_exp);return sp_str_bin_like(s,r);}
static const char*sp_str_capitalize_f(const char*s,uint32_t(*fu)(uint32_t),uint32_t(*fl)(uint32_t)){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("capitalize");if(sp_str_is_binary(s))return sp_str_case_bin(s,3);size_t l=sp_str_byte_len(s);char*r=sp_str_alloc_raw(l*3+1);size_t oi=0;int first=1;for(size_t i=0;i<l;){uint32_t cp;int n=sp_utf8_decode(s+i,&cp);i+=(size_t)n;if(first){
  /* the first character's titlecase ("ǆ" is "ǅ", "ß" is "Ss"); a Turkic
     mapping of i/I first */
  uint32_t u=fu(cp);
  if(u!=sp_uc_toupper(cp))oi+=(size_t)sp_utf8_encode(u,r+oi);
  else oi+=sp_uc_title_utf8(cp,r+oi);
  first=0;}
else{uint32_t lo=fl(cp);uint32_t to[3];int k;
  if(lo==cp&&(k=sp_uc_special_of(SP_UC_LOWER,cp,to))){for(int j=0;j<k;j++)oi+=(size_t)sp_utf8_encode(to[j],r+oi);}
  else oi+=(size_t)sp_utf8_encode(lo,r+oi);}}r[oi]=0;sp_str_set_len(r,oi);return r;}
const char*sp_str_capitalize(const char*s){return sp_str_capitalize_f(s,sp_uc_toupper,sp_uc_tolower);}
static uint32_t sp_uc_toupper_tr(uint32_t c){return c==0x69?0x130:c==0x131?0x49:sp_uc_toupper(c);}
static uint32_t sp_uc_tolower_tr(uint32_t c){return c==0x49||c==0x131?0x131:c==0x130?0x69:sp_uc_tolower(c);}
const char*sp_str_upcase_turkic(const char*s){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("upcase");return sp_str_case_map(s,sp_uc_toupper_tr,1);}
const char*sp_str_downcase_turkic(const char*s){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("downcase");return sp_str_case_map(s,sp_uc_tolower_tr,0);}
const char*sp_str_swapcase_turkic(const char*s){return sp_str_swapcase_f(s,sp_uc_toupper_tr,sp_uc_tolower_tr);}
const char*sp_str_capitalize_turkic(const char*s){return sp_str_capitalize_f(s,sp_uc_toupper_tr,sp_uc_tolower_tr);}
static const char*sp_sym_case_tr(const char*s,const char*(*t)(const char*),const char*(*f)(const char*)){for(const unsigned char*p=(const unsigned char*)s;p&&*p;p++)if(*p>=0x80)return t(s);return f(s);}
const char*sp_str_upcase_turkic_sym(const char*s){return sp_sym_case_tr(s,sp_str_upcase_turkic,sp_str_upcase);}
const char*sp_str_downcase_turkic_sym(const char*s){return sp_sym_case_tr(s,sp_str_downcase_turkic,sp_str_downcase);}
const char*sp_str_swapcase_turkic_sym(const char*s){return sp_sym_case_tr(s,sp_str_swapcase_turkic,sp_str_swapcase);}
const char*sp_str_capitalize_turkic_sym(const char*s){return sp_sym_case_tr(s,sp_str_capitalize_turkic,sp_str_capitalize);}
/* String#undump: reverse of String#dump. The argument must be wrapped in
   double quotes; the escapes dump can emit (\n \t \r \f \v \a \b \e \s \0
   \" \\ \# \xHH \uHHHH \u{...}) are decoded back to bytes. The decoded
   string is never longer than the dumped form, so one buffer suffices. */
const char*sp_str_undump(const char*s){SP_GC_ROOT_STR(s);
  if(!s)sp_nil_recv("undump");
  size_t n=strlen(s);
  if(n<2||s[0]!='"'||s[n-1]!='"'){sp_raise_cls("RuntimeError","invalid dumped string");return sp_str_empty;}
  const char*p=s+1;const char*pe=s+n-1;
  char*out=sp_str_alloc_raw(n+1);size_t oi=0;
  while(p<pe){
    if(*p!='\\'){out[oi++]=*p++;continue;}
    p++;if(p>=pe)break;
    char c=*p++;
    if(c=='n')out[oi++]='\n';else if(c=='t')out[oi++]='\t';else if(c=='r')out[oi++]='\r';
    else if(c=='f')out[oi++]='\f';else if(c=='v')out[oi++]='\v';else if(c=='a')out[oi++]='\a';
    else if(c=='b')out[oi++]='\b';else if(c=='e')out[oi++]='\033';else if(c=='s')out[oi++]=' ';
    else if(c=='0')out[oi++]='\0';else if(c=='\\')out[oi++]='\\';else if(c=='"')out[oi++]='"';
    else if(c=='#')out[oi++]='#';
    else if(c=='x'){int v=0,k=0;while(k<2&&p<pe&&isxdigit((unsigned char)*p)){v=(v*16)+_sp_hexval((unsigned char)*p);p++;k++;}out[oi++]=(char)v;}
    else if(c=='u'){
      if(p<pe&&*p=='{'){p++;while(p<pe&&*p!='}'){while(p<pe&&*p==' ')p++;uint32_t cp=0;int k=0;while(k<8&&p<pe&&isxdigit((unsigned char)*p)){cp=(cp*16)+(uint32_t)_sp_hexval((unsigned char)*p);p++;k++;}char enc[4];int el=sp_utf8_encode(cp,enc);for(int j=0;j<el;j++)out[oi++]=enc[j];while(p<pe&&*p==' ')p++;}if(p<pe&&*p=='}')p++;}
      else{uint32_t cp=0;int k=0;while(k<4&&p<pe&&isxdigit((unsigned char)*p)){cp=(cp*16)+(uint32_t)_sp_hexval((unsigned char)*p);p++;k++;}char enc[4];int el=sp_utf8_encode(cp,enc);for(int j=0;j<el;j++)out[oi++]=enc[j];}
    }
    else out[oi++]=c;
  }
  out[oi]=0;sp_str_set_len(out,oi);return out;
}
const char*sp_str_succ_impl(const char*s){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("succ");size_t l=sp_str_byte_len(s);if(l==0){char*r=sp_str_alloc_raw(1);r[0]=0;sp_str_set_len(r,0);return sp_str_bin_like(s,r);}/* Find start of last codepoint */size_t lc=l-1;while(lc>0&&((unsigned char)s[lc]&0xC0)==0x80)lc--;if((unsigned char)s[lc]>=0x80){/* Multibyte tail: increment its codepoint */uint32_t cp;sp_utf8_decode(s+lc,&cp);cp++;char enc[4];int el=sp_utf8_encode(cp,enc);char*r=sp_str_alloc_raw(lc+el+1);memcpy(r,s,lc);memcpy(r+lc,enc,el);r[lc+el]=0;sp_str_set_len(r,lc+(size_t)el);return sp_str_bin_like(s,r);}/* ASCII tail: CRuby's alnum-aware carry. The rightmost alphanumeric
   increments; a wrap (9->0, z->a, Z->A) carries into the adjacent character
   when it is alphanumeric of any class, else into the nearest alphanumeric
   to the left of the SAME class (digit vs alpha); with no carry target left,
   the wrapped class's carry character (1/a/A) is inserted at the wrap
   position. A string with no alphanumerics increments its last byte. */
char*r=sp_str_alloc_raw(l+2);memcpy(r,s,l);r[l]=0;
#define SP_SUCC_AL(ch) (((ch)>='0'&&(ch)<='9')||((ch)>='a'&&(ch)<='z')||((ch)>='A'&&(ch)<='Z'))
sp_int i=(sp_int)l-1;
while(i>=0&&!SP_SUCC_AL((unsigned char)r[i]))i--;
if(i<0){r[l-1]=(char)((unsigned char)r[l-1]+1);sp_str_set_len(r,l);return sp_str_bin_like(s,r);}
for(;;){
  unsigned char c=(unsigned char)r[i];
  if(c!='9'&&c!='z'&&c!='Z'){r[i]=(char)(c+1);sp_str_set_len(r,l);return sp_str_bin_like(s,r);}
  int dig=(c=='9');
  char ins=(c=='9')?'1':(c=='z')?'a':'A';
  r[i]=(c=='9')?'0':(c=='z')?'a':'A';
  if(i>0&&SP_SUCC_AL((unsigned char)r[i-1])){i--;continue;}
  sp_int j=i-1;
  while(j>=0&&!SP_SUCC_AL((unsigned char)r[j]))j--;
  if(j>=0){unsigned char cj=(unsigned char)r[j];int jdig=(cj>='0'&&cj<='9');if(jdig==dig){i=j;continue;}}
  memmove(r+i+1,r+i,l-(size_t)i+1);r[i]=ins;sp_str_set_len(r,l+1);return sp_str_bin_like(s,r);
}
#undef SP_SUCC_AL
}
/* The ASCII same-length carry paths in succ_impl allocate l+2 bytes (room for
   a prepend) but return a string of length l, leaving the heap header's len
   field one too large. Callers that read sp_str_byte_len (e.g. concat) then
   copy a trailing NUL. This wrapper normalizes the header len to strlen; succ
   never produces an embedded NUL so this is always correct. */
/* succ_impl records the real length on every path now, so the old strlen
   normalisation here would UNDO it for a source holding a NUL. */
const char*sp_str_succ(const char*s){SP_GC_ROOT_STR(s);return sp_str_succ_impl(s);}
/* succ applied n times (n < 1: the string itself): the next member of an
   endless String range walked by step(n) */
const char*sp_str_succ_n(const char*s,sp_int n){SP_GC_ROOT_STR(s);for(sp_int i=0;i<n;i++)s=sp_str_succ_impl(s);return s;}
/* String#gsub(pat, rep) for literal (non-regex) patterns. Issue #827: the
   result must come from sp_str_alloc, not a raw malloc buffer, because the
   GC's sp_mark_string writes the marker byte at offset -1 and would corrupt
   malloc metadata otherwise. Issue #850: an empty pattern inserts the
   replacement between every character (and at both ends). */
/* Expand backslash sequences in a gsub/sub replacement against a fixed matched
   text `match` (mlen bytes). For a String pattern there are no capture groups,
   so \1-\9 expand to nothing; \\ -> \, \& and \0 -> the whole match; any other
   \c keeps both characters. Returns a malloc'd C string the caller frees.
   Returns NULL when the replacement has no backslash (caller uses rep as-is). */
static char *sp_str_rep_expand(const char *rep, const char *match, size_t mlen, size_t *olen) {
  size_t rl = sp_str_byte_len(rep);
  if (!rep || !memchr(rep, '\\', rl)) return NULL;
  size_t cap = rl + mlen + 1, ol = 0;
  char *out = (char *)malloc(cap);
  for (size_t i = 0; i < rl; i++) {
    if (rep[i] == '\\' && i + 1 < rl) {
      char d = rep[i + 1]; i++;
      if (d == '\\') { if (ol + 1 >= cap) { cap = cap * 2 + 1; out = (char *)realloc(out, cap); } out[ol++] = '\\'; }
      else if (d == '&' || d == '0') { if (ol + mlen >= cap) { cap = (ol + mlen) * 2 + 1; out = (char *)realloc(out, cap); } memcpy(out + ol, match, mlen); ol += mlen; }
      else if (d >= '1' && d <= '9') { /* no capture groups: empty */ }
      else { if (ol + 2 >= cap) { cap = cap * 2 + 1; out = (char *)realloc(out, cap); } out[ol++] = '\\'; out[ol++] = d; }
    }
    else {
      if (ol + 1 >= cap) { cap = cap * 2 + 1; out = (char *)realloc(out, cap); }
      out[ol++] = rep[i];
    }
  }
  out[ol] = 0;
  *olen = ol;
  return out;
}
const char*sp_str_gsub(const char*s,const char*pat,const char*rep){SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(pat);SP_GC_ROOT_STR(rep);
  if(!s)sp_nil_recv("gsub");
  if(!pat||!rep)return sp_str_one_answer(s);
  /* byte lengths and a byte-exact search throughout: strlen/strstr stop at an
     embedded NUL, so a subject or pattern holding one was cut short (the
     opportunistic NUL policy). */
  size_t pl0=sp_str_byte_len(pat);
  size_t el=0;char*rep_exp=sp_str_rep_expand(rep,pat,pl0,&el);
  /* the expansion is malloc'd: it goes in a variable of its own, since `rep`
     is rooted as a String and the collector would read (and mark) the byte
     before a malloc'd block (#827) */
  const char*rp=rep_exp?rep_exp:rep;
  size_t pl=pl0,rl=rep_exp?el:sp_str_byte_len(rp),sl=sp_str_byte_len(s);
  if(pl==0){
    sp_re_sub_matched=1;
    /* Empty pattern: insert rep between every codepoint + at start/end.
       Result size: (chars+1) * rl + sl. */
    size_t cap=sl+(rl*(sl+1))+1;
    char*out=(char*)malloc(cap);
    size_t ol=0;
    memcpy(out+ol,rp,rl); ol+=rl;
    for(size_t i=0;i<sl;){
      int n=sp_utf8_advance(s+i); if(n<1)n=1; if((size_t)n>sl-i)n=(int)(sl-i);
      memcpy(out+ol,s+i,(size_t)n); ol+=(size_t)n;
      memcpy(out+ol,rp,rl); ol+=rl;
      i+=(size_t)n;
    }
    out[ol]=0;
    if(sp_re_track_last)sp_re_set_lit_match(s,(sp_int)sl,(sp_int)sl);   /* `$~`: the last, empty, match at the end */
    char*r=sp_str_alloc(ol); memcpy(r,out,ol); sp_str_set_len(r,ol); free(out); if(rep_exp)free(rep_exp); return sp_str_bin_like(s,r);
  }
  size_t cap=(sl*2)+1;
  char*out=(char*)malloc(cap);
  size_t ol=0;
  const char*p=s;const char*se=s+sl;const char*last=NULL;
  while(p<se){
    const char*f=sp_bytestr(p,(size_t)(se-p),pat,pl);
    if(!f){size_t n=(size_t)(se-p);if(ol+n>=cap){cap=((ol+n)*2)+1;out=(char*)realloc(out,cap);}memcpy(out+ol,p,n);ol+=n;break;}
    sp_re_sub_matched=1;
    last=f;
    size_t n=(size_t)(f-p);
    if(ol+n+rl>=cap){cap=((ol+n+rl)*2)+1;out=(char*)realloc(out,cap);}
    memcpy(out+ol,p,n);ol+=n;
    memcpy(out+ol,rp,rl);ol+=rl;
    p=f+pl;
  }
  /* `$~` is the last match, or nil when there was none */
  if(sp_re_track_last){if(last)sp_re_set_lit_match(s,(sp_int)(last-s),(sp_int)(last-s+pl));else sp_re_clear_last_match();}
  out[ol]=0;char*r=sp_str_alloc(ol);memcpy(r,out,ol);sp_str_set_len(r,ol);free(out);if(rep_exp)free(rep_exp);return sp_str_bin_like(s,r);
}
