#!/usr/bin/env python3
"""Verify derived inputs, unchanged spans, classified edits and explicit ABI use."""
import argparse
from bisect import bisect_right
from collections import Counter
import json
from pathlib import Path
import re
import derive_ppu_memory_context as derive

ARITY = {f"ppu_native_read{width}":2 for width in (8,16,32,64)}
ARITY.update({f"ppu_native_write{width}":3 for width in (8,16,32,64)})
ARITY.update({"ppu_native_read_bytes":4,"ppu_native_write_bytes":4,
              "ppu_native_load_reserve32":2,"ppu_native_load_reserve64":2,
              "ppu_native_store_conditional32":3,"ppu_native_store_conditional64":3,
              "ppu_native_atomic_add16":3,"ppu_native_zero128":2})
PREFIXES = {f"vm_read{width}":f"ppu_native_read{width}" for width in (8,16,32,64)}
PREFIXES.update({f"vm_write{width}":f"ppu_native_write{width}" for width in (8,16,32,64)})
PREFIXES.update({"ppu_atomic_add16":"ppu_native_atomic_add16","ppu_dcbz128":"ppu_native_zero128"})
FUNCTION_HEADER = re.compile(r"^void func_[0-9A-Fa-f]{8}\(ppu_context\* ctx\) \{$",re.MULTILINE)


def audit_native(source,name):
    lexer = derive.Lexer()
    depth,function_depth = 0,None
    coverage = []
    for number,line in enumerate(source.splitlines(keepends=True),1):
        tokens,directive = lexer.line(line)
        names = derive.spelling(tokens)
        opening = len(tokens) == 8 and names[0] == "void" and names[1].startswith("func_") and names[-1] == "{"
        if opening:
            derive.require(tokens[2:],"(ppu_context* ctx) {")
            function_depth = depth+1
            coverage.append(names[1])
        for index,token in enumerate(tokens):
            if token.literal:
                continue
            value = token.text
            if value in derive.LEGACY or value.startswith(("ppu__ld","ppu__st","ppu_slow_","__atomic_","__sync_")):
                raise derive.DerivationError(f"{name}:{number}: legacy residual {value}")
            if value.startswith("ppu_native_"):
                if directive or function_depth is None or value not in ARITY:
                    raise derive.DerivationError("native call outside admitted scope or unknown ABI")
                args,_ = derive.call_arguments(tokens,index)
                if len(args) != ARITY[value] or derive.spelling(args[0]) != ["ctx"]:
                    raise derive.DerivationError("native ABI arity/context mismatch")
        if directive:
            continue
        for token in tokens:
            if token.literal:
                continue
            if token.text == "{": depth += 1
            if token.text == "}":
                depth -= 1
                if depth < 0: raise derive.DerivationError("invalid derived scope")
                if function_depth is not None and depth < function_depth: function_depth = None
    lexer.finish()
    if depth or function_depth: raise derive.DerivationError("unclosed derived scope")
    return derive.function_coverage(coverage)


def check_edit_semantics(original,replacement,kind):
    old = original.decode("utf-8")
    new = replacement.decode("utf-8")
    tokens = derive.lex(old)
    if kind == "base_declaration":
        derive.require(tokens,'extern "C" uint8_t* vm_base;')
        if replacement: raise derive.DerivationError("base declaration not removed")
    elif kind == "context_call":
        if len(tokens) == 2 and tokens[1].text == "(" and tokens[0].text in PREFIXES:
            expected = PREFIXES[tokens[0].text]+"(ctx, "
        elif tokens and tokens[0].text == "ppu_stvx16":
            edits = derive.regular_calls(old,tokens)
            expected = derive.replace_local(old,edits)
        else:
            raise derive.DerivationError("context-call record is not a recognized call span")
        if new != expected: raise derive.DerivationError("incorrect contextual call edit")
    elif kind in ("lr32","lr64","sc32","sc64"):
        expected,classified = derive.reservation_block(old,tokens)
        if kind != classified or new != expected:
            raise derive.DerivationError("reservation block edit mismatch")
    elif kind in ("raw_left_load","raw_element_load","raw_vector_read","raw_vector_write","raw_reverse_read"):
        expected,classified = derive.raw_block(old,tokens)
        if kind != classified or new != expected:
            raise derive.DerivationError("raw block edit mismatch")
    else:
        raise derive.DerivationError("unknown edit class "+str(kind))


def check_source(original,output,records,name,coverage=None):
    derive.verify_edits(original,output,records)
    if FUNCTION_HEADER.findall(original.decode()) != FUNCTION_HEADER.findall(output.decode()):
        raise derive.DerivationError("generated function coverage changed")
    actual_coverage = audit_native(output.decode(),name)
    if coverage != actual_coverage:
        raise derive.DerivationError("manifest function coverage mismatch")
    starts = [0]+[match.end() for match in re.finditer(b"\n",original)]
    hits = {key:set() for key in derive.PATTERNS}
    classes = Counter()
    for record in records:
        start,old_size,new_start,new_size,kind,_,_ = record
        if old_size <= 0:
            raise derive.DerivationError("empty original edit span")
        old = original[start:start+old_size]
        new = output[new_start:new_start+new_size]
        check_edit_semantics(old,new,kind)
        line = bisect_right(starts,start)-1
        for key,pattern in derive.PATTERNS.items():
            if pattern.search(old.decode()): hits[key].add(line)
        classes["edit_"+kind] += 1
    return {**{key:len(value) for key,value in hits.items()},**classes}


def check_directory(output,project_root=derive.ROOT,allow_fixture=False,fixture_source=None,fixture_header=None):
    output,project_root = Path(output),Path(project_root)
    manifest_path = output/"manifest.json"
    if manifest_path.is_symlink(): raise derive.DerivationError("symlinked manifest")
    manifest = json.loads(manifest_path.read_text())
    if manifest.get("schema") != 1 or manifest.get("scope") not in ("production","fixture_only"):
        raise derive.DerivationError("unknown manifest schema/scope")
    fixture = manifest["scope"] == "fixture_only"
    if fixture:
        if not allow_fixture or fixture_source is None or fixture_header is None:
            raise derive.DerivationError("fixture cannot be accepted as production")
        sources = {"ppu_recomp.h":Path(fixture_header),Path(fixture_source).name:Path(fixture_source)}
    else:
        sources = {name:project_root/derive.SOURCE_PREFIX/name for name in derive.PINS}
    expected_outputs = set(sources)|{"native_memory_abi.h"}
    if set(manifest.get("original_inputs",{})) != set(sources) or set(manifest.get("outputs",{})) != expected_outputs:
        raise derive.DerivationError("manifest names/path scope mismatch")
    if {path.name for path in output.iterdir()} != expected_outputs|{"manifest.json"}:
        raise derive.DerivationError("unexpected derived files")
    if manifest.get("abi_sha256") != derive.ABI_SHA256 or manifest.get("tool_sha256") != derive.sha(Path(derive.__file__).read_bytes()):
        raise derive.DerivationError("tool/ABI provenance mismatch")
    if derive.sha((project_root/derive.ABI_PATH).read_bytes()) != derive.ABI_SHA256:
        raise derive.DerivationError("live ABI contract changed")
    checked = {}
    for name in sorted(expected_outputs):
        path = output/name
        if path.is_symlink() or not path.is_file(): raise derive.DerivationError("invalid output file")
        data = path.read_bytes()
        info = manifest["outputs"][name]
        if derive.sha(data) != info.get("sha256") or len(data) != info.get("bytes"):
            raise derive.DerivationError("derived hash/length mismatch "+name)
        if name == "native_memory_abi.h":
            if derive.sha(data) != derive.ABI_SHA256 or info.get("edits") != []:
                raise derive.DerivationError("ABI copy mismatch")
            continue
        original = sources[name].read_bytes()
        expected = manifest["original_inputs"][name]
        if derive.sha(original) != expected.get("sha256") or len(original) != expected.get("bytes"):
            raise derive.DerivationError("source changed "+name)
        if not fixture and expected["sha256"] != derive.PINS[name]:
            raise derive.DerivationError("source not baseline pin")
        records = info.get("edits",[])
        if name == "ppu_recomp.h":
            expected_header,expected_edits,before,after = derive.derive_header(original.decode())
            if expected_header.encode() != data or expected_edits != records or before != after:
                raise derive.DerivationError("derived header/CPU bytes mismatch")
            if manifest.get("cpu_type_original_sha256") != before or manifest.get("cpu_type_derived_sha256") != after:
                raise derive.DerivationError("CPU provenance mismatch")
            checked[name] = {"edits":len(records),"cpu_type_unchanged":True}
        else:
            print("CONTEXT_CHECK_UNIT_BEGIN "+name,flush=True)
            counts = check_source(original,data,records,name,info.get("function_coverage"))
            if counts != info.get("counts"):
                raise derive.DerivationError("edit/census accounting mismatch "+name)
            if not fixture and any(counts[key] != value for key,value in zip(derive.PATTERNS,derive.EXPECTED[Path(name).stem[-3:]])):
                raise derive.DerivationError("baseline census mismatch "+name)
            checked[name] = {"edits":len(records),"counts":counts}
            print("CONTEXT_CHECK_UNIT_PASS "+name,flush=True)
    return {"scope":manifest["scope"],"manifest_sha256":derive.sha(manifest_path.read_bytes()),"checked":checked}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory",type=Path)
    parser.add_argument("--project-root",type=Path,default=derive.ROOT)
    parser.add_argument("--allow-fixture",action="store_true")
    parser.add_argument("--fixture-source",type=Path)
    parser.add_argument("--fixture-header",type=Path)
    parser.add_argument("--report",type=Path)
    args = parser.parse_args()
    try:
        derive.apply_process_limits(900,4096,2)
        report = check_directory(args.directory,args.project_root,args.allow_fixture,args.fixture_source,args.fixture_header)
        if args.report:
            with args.report.open("x") as stream: json.dump(report,stream,indent=2); stream.write("\n")
    except (derive.DerivationError,OSError,ValueError,TypeError,KeyError,MemoryError) as error:
        parser.exit(1,f"CONTEXT_CHECK_FAIL {error}\n")
    print("CONTEXT_CHECK_PASS scope="+report["scope"])

if __name__ == "__main__": main()
