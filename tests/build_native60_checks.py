"""Rebuild native-60 checks against the current SDL3 moddable object files.
Requires private CPU/RAM captures from the local game; none are distributed.
"""
from pathlib import Path
import argparse, importlib.util, os, subprocess
p=argparse.ArgumentParser()
for name in ('cc','sdk','cpu','ram'):p.add_argument('--'+name,required=True)
a=p.parse_args()
cc,sdk,cpu,ram=(Path(getattr(a,n)).resolve() for n in ('cc','sdk','cpu','ram'))
root=Path(__file__).resolve().parents[1]
out=root/'build/native60-checks';out.mkdir(exist_ok=True)
os.environ['PATH']=str(sdk/'bin')+os.pathsep+os.environ['PATH']
os.environ['ACX_TEST_CONTROLLER_DB']=str(Path(__file__).resolve().parents[1]/'packaging/windows/gamecontrollerdb.txt')
s=importlib.util.spec_from_file_location('acx_build',root/'build.py')
b=importlib.util.module_from_spec(s);s.loader.exec_module(b)
def compile(src,dest,defs=()):
 _,rc,log=b.compile_one(str(cc),src,dest,list(defs))
 if log:print(log)
 if rc:raise SystemExit(rc)
def run(cmd):subprocess.run([str(x) for x in cmd],check=True)
def check(exe,args=()):
 r=subprocess.run([str(exe),*map(str,args)],capture_output=True,text=True)
 (out/(exe.stem+'.txt')).write_text(r.stdout+r.stderr)
 print(exe.stem+': '+('PASS' if r.returncode==0 else 'FAIL'))
 if r.returncode:raise SystemExit(r.returncode)
timing=out/'timing.exe'
run([cc,*b.CFLAGS,root/'tests/framerate_regression.c',root/'runtime/hle/framerate.c','-o',timing])
check(timing)
objects=list((root/'build/obj-sdl3').glob('*.o'))
if not objects:raise SystemExit('First build the SDL3 moddable runtime.')
libs=['-static-libgcc','-L',str(sdk/'lib'),'-lSDL3','-lvulkan-1','-Wl,-Bstatic','-lstdc++','-lwinpthread','-Wl,-Bdynamic','-lwinmm','-luser32','-lmfplat','-lmfuuid','-lole32']
for name in ('speed_integrator_regression','gravity_regression','axis_regression','flight_alignment_regression','controls_regression','controller_mapping_regression'):
 obj=out/(name+'.o');exe=out/(name+'.exe')
 compile(root/'tests'/ (name+'.c'),obj,['-DPSPRECO_MODDABLE','-I',str(sdk/'include')])
 inputs=[o for o in objects if o.name!='main.o']
 if name in ('axis_regression','flight_alignment_regression','controls_regression'):
  replacement=out/'framerate-clock.o'
  compile(root/'runtime/hle/framerate.c',replacement,['-DPSPRECO_MODDABLE','-Dthreadman_vblank_count=test_vblank_count'])
  inputs=[o for o in inputs if o.name!='framerate.o']+[replacement]
 if name=='controls_regression':
  presenter=out/'present-test.o'
  compile(root/'runtime/host/present_null.c',presenter,['-Dpresent_aux=unused_present_aux','-Dpresent_rudder=unused_present_rudder'])
  inputs=[o for o in inputs if o.name!='present_sdl3vk.o']+[presenter]
 if name=='controller_mapping_regression':
  presenter=out/'present-sdl-test.o'
  compile(root/'runtime/host/present_sdl3vk.c',presenter,['-DPSPRECO_SDL3','-DPSPRECO_INPUT_TEST','-I',str(sdk/'include'),'-I',str(root/'third_party/imgui')])
  inputs=[o for o in inputs if o.name!='present_sdl3vk.o']+[presenter]
  bindings=out/'input-bindings-test.o'
  compile(root/'runtime/host/input_bindings.c',bindings,['-DPSPRECO_INPUT_TEST','-I',str(sdk/'include')])
  inputs=[o for o in inputs if o.name!='input_bindings.o']+[bindings]
 run([cc,*inputs,obj,'-o',exe,*libs])
 check(exe,[cpu,ram])
print('All native-60 checks passed. Results: '+str(out))
