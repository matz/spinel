#!/usr/bin/env python3
# cnorm.py A.c B.c: whether two compiles of one program differ only in what a change of local-variable names moves.
# CNORM_ROOTS=tree1:tree2 names the checkouts whose paths are mapped to TREE. Prints the first level at which the files agree:
#   alpha   per function, the `lv_*`/`_kwr_*`/`_y<N>_*`/capture names renumbered by first occurrence are equal (names only)
#   strip   equal also after stripping the scope tag a name carries (`B<n>_<tag>_x`, `P_`, `D<n>_`, `__dup<n>`)
#   order   the same lines per function, in another order (declaration order)
#   slots N a slot, its GC-frame entry or its declaration is added or dropped (N lines)
#   ints    equal up to integer literals (frame sizes)
#   differs N  anything else (N lines)
# The symbol table's lines and the revision in the version string are dropped before comparing.
import re,sys
# the symbol table's records, whole lines: names, hash table and its continuation rows, mask, and the helpers over them
SYM=re.compile(r'^(?:static [^=]*\b(?:sp_sym_names|sp_sym_htab|sp_dyn_sym\w*|sp_dsym\w*)\b|static [^=(]*\bsp_sym_\w+\(|#define SP_SYM_HMASK\b|#define SP_DYN_SYM\w*|0[,x])')
ID=re.compile(r'\b(?:lv_|_kwr_|_y\d+_|_cell_|c_|gv___dmcap)[A-Za-z0-9_]+')
SUF=re.compile(r'(__b[pq]?[A-Za-z_]*\d+|__cb\d+|__b\d+|__dup\d*|__bpin)(?=\b|_)')
SLOT=re.compile(r'(?:(?<=_)|^)((?:P_|D\d+_)*)B(\d+)_')
def unslot(k):
    # `B<n>_<tag>_x` (a tag of n characters): the slot of a local x
    out=''; i=0
    while True:
        m=SLOT.search(k,i)
        if not m: return out+k[i:]
        e=m.end()+int(m.group(2))
        if e<len(k) and k[e]=='_': out+=k[i:m.start()]; i=e+1
        else: out+=k[i:m.end()]; i=m.end()
import os
ROOTS=[r for r in os.environ.get('CNORM_ROOTS','').split(':') if r]
LIT=re.compile(r'"(?:[^"\\]|\\.)*"|\'(?:[^\'\\]|\\.)*\'')
def symnum(line):
    # a symbol's number, which the symbol table's entries move, in the code of a line: a string or character
    # literal keeps its text, so two literals that differ stay different
    out=[]; i=0
    for m in LIT.finditer(line):
        out.append(re.sub(r'\(sp_sym\)\d+','(sp_sym)N',re.sub(r'sp_box_sym\(\d+\)','sp_box_sym(N)',line[i:m.start()])))
        out.append(m.group(0)); i=m.end()
    out.append(re.sub(r'\(sp_sym\)\d+','(sp_sym)N',re.sub(r'sp_box_sym\(\d+\)','sp_box_sym(N)',line[i:])))
    return ''.join(out)
def lines(p):
    o=[]
    # a backslash-newline joins its two lines before anything else (translation phase 2), so a literal or a
    # macro a line break continues is one logical line here; the counts below are of logical lines
    for ln in re.sub(r'\\\n','',open(p,errors='replace').read()).splitlines():
        if SYM.search(ln): continue
        for r in ROOTS: ln=ln.replace(r,'TREE')
        ln=symnum(ln)
        ln=re.sub(r'\+[0-9]+ revision [0-9a-f]+','+N revision R',ln)
        o.append(ln)
    return o
def alpha(ls,strip):
    m={}; o=[]
    for l in ls:
        if l[:1] not in ' \t}#' and (l.rstrip().endswith('{') or l.rstrip().endswith(');')): m={}   # a function, or a prototype
        def r(x):
            k=x.group(0)
            if strip: k=SUF.sub('',unslot(k))
            if k not in m: m[k]='ID%d'%len(m)
            return m[k]
        o.append(ID.sub(r,l))
    return o
a,b=lines(sys.argv[1]),lines(sys.argv[2])
if alpha(a,0)==alpha(b,0): print('alpha'); sys.exit()
A,B=alpha(a,1),alpha(b,1)
if A==B: print('strip'); sys.exit()
def fnsort(ls):
    out=[];cur=[];
    for l in ls:
        if l[:1] not in ' \t}#' and l.rstrip().endswith('{'):
            out.append(sorted(cur)); cur=[l]
        else: cur.append(l)
    out.append(sorted(cur)); return out
if fnsort(A)==fnsort(B): print('order'); sys.exit()
import collections
def ms(ls): return collections.Counter(re.sub(r'\b\d+\b','N',re.sub(r'ID\d+','ID',x)) for x in ls)
da=ms(A)-ms(B); db=ms(B)-ms(A)
# a slot line: a declaration of a local (a type, then the local), its GC root or frame entry, or `(void) lv_x;`; a
# statement that only reads or assigns one is not
SLOTLN=re.compile(r'^\s*(?:(?!return\b|goto\b|case\b|else\b)[A-Za-z_]\w*(?:\s+|\s*\*+\s*)\**(?:ID|lv_\w+|_cell_\w+)\s*(?:\[[^\]]*\])?\s*(?:=[^;]*)?;|\(void\)\s*(?:ID|lv_\w+);|.*\bSP_GC_(?:ROOT\w*|ENTRY\w*|SAVE)\b|.*\bsp_gc_frame_hdr\b)')
if (da or db) and all(SLOTLN.search(x) for x in list(da)+list(db)): print('slots %d'%(sum(da.values())+sum(db.values()))); sys.exit()
num=lambda l: re.sub(r'\b\d+\b','N',l)
if [num(x) for x in A]==[num(x) for x in B]: print('ints'); sys.exit()
import difflib
print('differs %d'%sum(1 for l in difflib.unified_diff(A,B,lineterm='',n=0) if l[:1] in '+-' and l[:3] not in ('---','+++')))
