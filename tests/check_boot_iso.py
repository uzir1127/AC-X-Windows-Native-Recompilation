"""Compare the native ISO importer with Python's independent ELF loader.

Private ISO is supplied at test time; never packaged. Negative fixtures contain
only a disc header or a local, short prefix, and remain in the test output.
"""
from pathlib import Path
import argparse, hashlib, importlib.util, subprocess, sys
p=argparse.ArgumentParser();p.add_argument('--cc',required=True);p.add_argument('--iso',type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[1]
out=root/'build/boot-iso-checks';out.mkdir(parents=True,exist_ok=True)
exe=out/'import-check.exe'
subprocess.run([a.cc,'-std=c11','-O2','-Wall','-Wextra','-I',str(root/'runtime/cpu'),'-I',str(root/'runtime/hle'),
 str(root/'tests/boot_iso_check.c'),str(root/'runtime/hle/boot_iso.c'),str(root/'runtime/hle/iso9660.c'),'-o',str(exe)],check=True)

def run(file,expect=0,dest=None):
 r=subprocess.run([str(exe),str(file),*([str(dest)] if dest else [])],capture_output=True,text=True,timeout=20)
 assert r.returncode==expect,(file,r.returncode,r.stdout,r.stderr)
 return r.stdout+r.stderr

native=out/'native-image.bin';run(a.iso,dest=native)
sys.path.insert(0,str(root/'tools/prxload'))
from prxload import PrxModule,PSP_USER_MODULE_BASE
# Read the executable using an independent Python ISO walker.
spec=importlib.util.spec_from_file_location('isoextract',root/'tools/isoextract.py')
i=importlib.util.module_from_spec(spec);spec.loader.exec_module(i)
with a.iso.open('rb') as f:
 import struct
 pvd=i.read_sector(f,16);records=[]
 i.walk(f,struct.unpack_from('<I',pvd,158)[0],struct.unpack_from('<I',pvd,166)[0],'',records)
 lba,size,_=next(r for r in records if r[2]=='PSP_GAME/SYSDIR/BOOT.BIN')
 f.seek(lba*2048);boot=f.read(size)
elf=out/'BOOT.elf';elf.write_bytes(boot)
reference,_=PrxModule(elf).build_image(rebase=PSP_USER_MODULE_BASE)
assert native.read_bytes()==reference,'Native relocation differs from Python prxload'
print('PASS byte-for-byte native/Python image:',hashlib.sha256(reference).hexdigest())
# Prefix holds all directory tables and BOOT, but deliberately lacks the CDI.
with a.iso.open('rb') as f: prefix=f.read((lba*2048+size+2047)//2048*2048)
short=out/'truncated.iso';short.write_bytes(prefix)
assert 'incomplete' in run(short,2)
modified=bytearray(prefix);modified[lba*2048+0x100]^=1
bad=out/'modified.iso';bad.write_bytes(modified)
assert 'Unsupported or modified' in run(bad,2)
missing=out/'not-iso.iso';missing.write_bytes(b'not an ISO')
assert 'Cannot mount' in run(missing,2)
# A descriptor with an invalid short record must be rejected safely.
invalid=bytearray(20*2048);invalid[16*2048]=1;invalid[16*2048+1:16*2048+6]=b'CD001'
struct.pack_into('<I',invalid,16*2048+158,18);struct.pack_into('<I',invalid,16*2048+166,2048)
invalid[18*2048]=3
badrec=out/'short-record.iso';badrec.write_bytes(invalid)
assert 'Unsupported game' in run(badrec,2)
print('PASS truncated ISO, changed executable, non-ISO, malformed directory record')
