from pathlib import Path
import hashlib,json,re
import os,sys
# usage: gen_wws_image.py <output dir>   (reads <repo>/infamous-recomp/spu/pm_wwsjob/ regenerated from your game)
root=Path(__file__).resolve().parents[2]
out_dir=Path(sys.argv[1]) if len(sys.argv)>1 else Path('.')
out_dir.mkdir(parents=True,exist_ok=True)
source=root/'infamous-recomp/spu/pm_wwsjob/spu_recomp.c'
header=source.with_suffix('.h')
expected='fe3c3d98a84998f2f5137bdb814ba7b41e252dba9528da8f0510dd57b8d1e42b'
raw=source.read_bytes()
assert hashlib.sha256(raw).hexdigest()==expected
assert hashlib.sha256(header.read_bytes()).hexdigest()=='648a753a2d12071fd7b844bf3a608ae97b29e0a623017aca864c40733e1714c7'
text=raw.decode()
start=text.index('spu_function_table[]')
end=text.index('};',start)
pairs=re.findall(r'\{\s*(0x[0-9A-Fa-f]+)[uU]?\s*,\s*(pm_wwsjob_spu_func_[0-9A-Fa-f]+)\s*,',text[start:end])
assert len(pairs)>100 and len(pairs)==len(set(pc for pc,fn in pairs))
pairs.sort(key=lambda pair:int(pair[0],16))
assert dict(pairs)['0x00002BF0']=='pm_wwsjob_spu_func_00002BF0' or any(int(pc,16)==0x2bf0 and fn=='pm_wwsjob_spu_func_00002BF0' for pc,fn in pairs)
output=out_dir/'wws_linked_image.c'
content='#include "spu_linked_image.h"\n#include "spu_recomp.h"\nstatic const ps3_spu_lifted_function functions[]={\n'
content+=''.join('    {%s,%s},\n'%(pc,fn) for pc,fn in pairs)
content+='};\nconst ps3_spu_linked_image ps3_native_wws_linked_image={"pm_wwsjob",functions,sizeof(functions)/sizeof(functions[0])};\n'
output.write_text(content)
receipt={'source':str(source.relative_to(root)),'source_sha256':expected,'header_sha256':hashlib.sha256(header.read_bytes()).hexdigest(),'table':output.name,'table_sha256':hashlib.sha256(output.read_bytes()).hexdigest(),'functions':len(pairs),'entry':'pm_wwsjob_spu_func_00002BF0','entry_pc':'0x2BF0','image_id':1}
(out_dir/'wws-image-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
print(json.dumps(receipt))
