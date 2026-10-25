#!/usr/bin/env python3
"""Recover omitted mod SDK metadata from a locally supplied symbol-bearing executable.
Run after emit_c.py: python tools/recover_mod_metadata.py --exe ORIGINAL.exe --out generated
No game code or data image is emitted by this utility.
"""
import argparse,struct,json,re
from pathlib import Path
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument("--exe",type=Path,required=True)
ap.add_argument("--out",type=Path,default=Path("generated"))
a=ap.parse_args()
b=a.exe.read_bytes()
pe=struct.unpack_from("<I",b,0x3c)[0]
_,nsec,_,symptr,nsyms,optsz,_=struct.unpack_from("<HHIIIHH",b,pe+4)
if not symptr or not nsyms:raise SystemExit("Executable has no COFF symbol table; use the original supplied build")
sections=[]
for i in range(nsec):
 o=pe+24+optsz+i*40
 vs,va,rs,ro=struct.unpack_from("<IIII",b,o+8)
 sections.append((va,ro,rs))
strings=symptr+nsyms*18
syms={};i=0
while i<nsyms:
 o=symptr+i*18;name=b[o:o+8]
 if name[:4]==bytes(4):
  j=strings+struct.unpack_from("<I",name,4)[0];name=b[j:b.index(bytes(1),j)]
 else:name=name.rstrip(bytes(1))
 val,sec,typ,sc,aux=struct.unpack_from("<IhHBB",b,o+8)
 if 0<sec<=len(sections):
  va,ro,rs=sections[sec-1];syms[name.decode(errors="replace")]=(ro+val,va+val)
 i+=1+aux
for name in ("g_acx_syms","g_acx_sym_count","g_acx_tables","g_acx_table_count"):
 if name not in syms:raise SystemExit("Missing symbol: "+name)
p=struct.unpack_from('<I',b,60)[0];base=struct.unpack_from('<Q',b,p+48)[0];n=struct.unpack_from('<H',b,p+6)[0];osz=struct.unpack_from('<H',b,p+20)[0];secs=[]
for i in range(n):
 o=p+24+osz+i*40;vs,va,rs,ro=struct.unpack_from('<IIII',b,o+8);secs.append((va,ro,rs))
def off(ptr):
 va=ptr-base
 for a,z,n in secs:
  if a<=va<a+n:return z+va-a
 raise ValueError(hex(ptr))
def cstr(ptr):
 if not ptr:return ''
 o=off(ptr);return b[o:b.index(bytes(1),o)].decode('utf8')
def cs(s):return json.dumps(s,ensure_ascii=True)
g=a.out
g.mkdir(parents=True,exist_ok=True)
header='/* Recovered from the user-supplied executable: absent upstream build metadata. */\n'
(g/'acx_symtab.h').write_text(header+'#include "pspreco.h"\nenum { ACX_SYM_FUNC = 0, ACX_SYM_GLOBAL = 1 };\ntypedef struct { const char *name; u32 guest, size; int kind; const char *hook; int recompiled; } AcxSym;\nextern const AcxSym g_acx_syms[];\nextern const int g_acx_sym_count;\n')
count=struct.unpack_from('<I',b,syms['g_acx_sym_count'][0])[0];o=syms['g_acx_syms'][0];rows=[];defs=[];symbols=[]
for i in range(count):
 name,addr,size,kind,hook,recomp=struct.unpack_from('<QIII4xQi4x',b,o+i*40);name=cstr(name);hook=cstr(hook)
 symbols.append(dict(name=name,addr=addr,size=size,kind=kind,hook=hook,recompiled=recomp))
 rows.append('{'+f'{cs(name)}, 0x{addr:08x}u, {size}, {kind}, {cs(hook)}, {recomp}'+'}')
 defs.append(f'#define G_{re.sub("[^a-zA-Z0-9_]","_",name[2:] if name.startswith("g_") else name).upper()} 0x{addr:08x}u')
(g/'acx_symtab.c').write_text(header+'#include "acx_symtab.h"\nconst AcxSym g_acx_syms[] = {\n'+',\n'.join(rows)+'\n};\nconst int g_acx_sym_count = '+str(count)+';\n')
(g/'acx_symbols.h').write_text(header+'#ifndef ACX_SYMBOLS_H\n#define ACX_SYMBOLS_H\n'+'\n'.join(defs)+'\n#endif\n')
(g/'guest-symbols.json').write_text(json.dumps(symbols,indent=2))
print('Recovered',count,'guest symbols')
count=struct.unpack_from('<I',b,syms['g_acx_table_count'][0])[0];o=syms['g_acx_tables'][0];tables=[];out=[header,'#include "datatables.h"\n']
for i in range(count):
 record,title,stride,head,floats,fields,nfields,live,note,aliases,listview=struct.unpack_from('<QQIII4xQiiQQi4x',b,o+i*72)
 record,title,note,aliases=map(cstr,(record,title,note,aliases));print(record,nfields,'stride',stride)
 fr=[]
 for j in range(nfields):
  fo,ft,fn,nt,traced,scale,unit,length=struct.unpack_from('<IiQQi4xdQI4x',b,off(fields)+j*56)
  fr.append('{'+f'{fo}, {ft}, {cs(cstr(fn))}, {cs(cstr(nt))}, {traced}, {scale!r}, {cs(cstr(unit))}, {length}'+'}')
 out.append('static const TableField fields_'+str(i)+'[] = {\n'+',\n'.join(fr)+'\n};\n')
 tables.append('{'+f'{cs(record)}, {cs(title)}, {stride}, {head}, {floats}, fields_{i}, {nfields}, {live}, {cs(note)}, {cs(aliases)}, {listview}'+'}')
out.append('const AcxTable g_acx_tables[] = {\n'+',\n'.join(tables)+'\n};\nconst int g_acx_table_count = '+str(count)+';\n');(g/'acx_tables.c').write_text('\n'.join(out))
print('Recovered',count,'table schemas')