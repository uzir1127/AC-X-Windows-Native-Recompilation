"""Read-only observer and SDL virtual-rumble lifecycle regressions."""
from pathlib import Path
import argparse, importlib.util, os, subprocess
p=argparse.ArgumentParser()
p.add_argument('--cc',required=True);p.add_argument('--sdk',required=True)
a=p.parse_args();cc=Path(a.cc).resolve();sdk=Path(a.sdk).resolve()
root=Path(__file__).resolve().parents[1];out=root/'build/rumble-checks';out.mkdir(exist_ok=True)
os.environ['PATH']=str(sdk/'bin')+os.pathsep+os.environ['PATH']
s=importlib.util.spec_from_file_location('acx_build',root/'build.py');b=importlib.util.module_from_spec(s);s.loader.exec_module(b)
def run(args):subprocess.run(list(map(str,args)),check=True)
def compile(src,obj,defs=()):
 _,rc,log=b.compile_one(str(cc),src,obj,list(defs))
 if log:print(log)
 if rc:raise SystemExit(rc)
exe=out/'observer.exe'
run([cc,*b.CFLAGS,root/'tests/rumble_observer_regression.c',root/'runtime/hle/rumble.c','-o',exe]);run([exe])
inputs=[o for o in (root/'build/obj-sdl3').glob('*.o') if o.name not in ('main.o','present_sdl3vk.o')]
presenter=out/'present-rumble-test.o';test=out/'rumble-test.o'
compile(root/'runtime/host/present_sdl3vk.c',presenter,['-DPSPRECO_SDL3','-DPSPRECO_INPUT_TEST','-Dui_visible=rumble_test_ui_visible','-I',str(sdk/'include'),'-I',str(root/'third_party/imgui')])
compile(root/'tests/rumble_sdl_regression.c',test,['-I',str(sdk/'include')])
exe=out/'sdl-rumble.exe'
libs=['-static-libgcc','-L',sdk/'lib','-lSDL3','-lvulkan-1','-Wl,-Bstatic','-lstdc++','-lwinpthread','-Wl,-Bdynamic','-lwinmm','-luser32','-lmfplat','-lmfuuid','-lole32']
run([cc,*inputs,presenter,test,'-o',exe,*libs]);run([exe])
print('Observer and SDL rumble checks PASS')
