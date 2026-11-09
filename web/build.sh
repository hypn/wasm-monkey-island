#!/bin/bash
# Builds web/dist/index.html: a single self-contained file (wasm and demo data embedded as base64),
# which therefore also works when opened straight from disk (file://).
set -e
cd "$(dirname "$0")"
ENG=../engine
DEMO=../mi_demo
mkdir -p dist build
emcc -O2 -DJS_CURSOR -sSINGLE_FILE=1 -sMODULARIZE=1 -sEXPORT_NAME=MI -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB -sMAXIMUM_MEMORY=1GB -sSTACK_SIZE=1MB --profiling-funcs \
  -sENVIRONMENT=web -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPF32 -sNO_EXIT_RUNTIME=1 \
  $ENG/main_wasm.c $ENG/vm.c $ENG/engine.c $ENG/actor.c $ENG/walk.c $ENG/costume.c $ENG/gfx.c $ENG/res.c $ENG/ui.c $ENG/sound.c $ENG/opl.c $ENG/state.c $ENG/menu.c \
  -o build/mi.js
python3 - <<PY
import base64,json
def b(p): return base64.b64encode(open(p,'rb').read()).decode()
d='$DEMO/'
data={'lec':b(d+'DISK01.LEC'),'idx':b(d+'000.LFL'),'charsets':{'1':b(d+'901.LFL'),'2':b(d+'902.LFL'),'4':b(d+'904.LFL')}}
t=open('index.template.html').read()
t=t.replace('/*__DATA__*/','const MI_DATA='+json.dumps(data)+';')
import datetime
t=t.replace('__BUILD__',datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S'))
t=t.replace('/*__MI_JS__*/',open('build/mi.js').read())
open('dist/index.html','w').write(t)
print('wrote dist/index.html',len(t),'bytes')
PY
# retail game files (not embedded, not redistributed): reachable as dist/retail/ for ?retail=true when served over http
ln -sfn ../retail dist/retail
