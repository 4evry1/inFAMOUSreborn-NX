#!/usr/bin/env python3
"""Hash-pinned, token/scope-aware native PPU memory derivation."""
from __future__ import annotations
import argparse
from collections import Counter
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import shutil
import tempfile

# tools/ lives in <repo>/switch/tools; the project root is the repository checkout
ROOT = Path(__file__).resolve().parents[2]
SOURCE_PREFIX = "infamous-recomp/recompiled/"
PINS = {
    "ppu_recomp_000.cpp": "98ff1a9b2b1814c8ab1179eedb2e3e3d7072c16a1151b52e8b7d7da2ad3b5358",
    "ppu_recomp_001.cpp": "0a1d0ad5a1a3c9f9de37d3eb50efe04f68291c6b56ad61f6e2fc692e53131e87",
    "ppu_recomp_002.cpp": "2a0732ff91791ff7c5452c1e3223740546fa8f2437317d1bf693a487d050c212",
    "ppu_recomp_003.cpp": "8f9838c8aa1e48ef5a4868a12ee182055c1708ad114724b2c21be861c446221e",
    "ppu_recomp_004.cpp": "df572c7644023d622e261f5aa571c43443dc566742e674bd5e9d21324b592564",
    "ppu_recomp.h": "e4be2060d1df2bced2215669a21f4929c89af9e32cced271ae8bb0a1d87556f0",
}
ABI_PATH = "ps3recomp/runtime/ppu/native_memory_abi.h"
ABI_SHA256 = "e7355edba7dede19196aa0b92204b978de89b565d05b0bfbdcc594a9175e000c"
PATTERNS = {
    "scalar_read": re.compile(r"\bvm_read(?:8|16|32|64)\s*\("),
    "scalar_write": re.compile(r"\bvm_write(?:8|16|32|64)\s*\("),
    "raw_vm_base": re.compile(r"\bvm_base\b"),
    "ll_reserve_value": re.compile(r"ctx->reserve_value\s*=(?!=)"),
    "sc32": re.compile(r"\bppu_stwcx32\s*\("),
    "sc64": re.compile(r"\bppu_stdcx64\s*\("),
    "atomic_add16": re.compile(r"\bppu_atomic_add16\s*\("),
    "dcbz_helper": re.compile(r"\bppu_dcbz128\s*\("),
    "vector_store_helper": re.compile(r"\bppu_stvx16\s*\("),
    "raw_alias": re.compile(r"uint8_t\s*\*\s*m\s*=\s*vm_base"),
}
EXPECTED = {
    "000": [75102,77000,17691,2,2,0,0,0,13426,0],
    "001": [79366,74594,14936,61,46,15,2,4,11386,0],
    "002": [77341,79300,8576,70,58,13,0,0,7093,6],
    "003": [78297,82770,14565,47,44,3,1,21,11924,0],
    "004": [40467,26721,1038,66,10,57,0,0,725,6],
}
LEGACY = {"vm_base", "PPU_VMB", "rc_vmb", "ppu_stwcx32", "ppu_stdcx64",
          "ppu_stvx16", "ppu_dcbz128", "ppu_atomic_add16", "vm_ptr"}
LEGACY.update(f"vm_{op}{width}" for op in ("read","write") for width in (8,16,32,64))
RESERVES = {"reserve_addr", "reserve_value", "reserve_valid"}
IDENT = re.compile(r"[A-Za-z_][A-Za-z_0-9]*")
NUMBER = re.compile(r"(?:0[xX][0-9A-Fa-f]+|[0-9]+)[uUlLfF]*")
RAW_START = re.compile(r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\(')
OPERATORS = (">>=","<<=","->*","...","::","->","++","--","&&","||","==","!=","<=",">=","<<",">>","+=","-=","*=","/=","&=","|=","^=")

class DerivationError(ValueError):
    pass

@dataclass(frozen=True, slots=True)
class Token:
    text: str
    start: int
    end: int
    literal: bool = False

class Lexer:
    """Persistent lexical regions; directives do not change C++ brace scope."""
    def __init__(self):
        self.comment = False
        self.raw_end = None
        self.quote = None
        self.directive_continuation = False

    def line(self, source):
        directive = self.directive_continuation or source.lstrip().startswith("#")
        self.directive_continuation = directive and source.rstrip().endswith("\\")
        if re.search(r"\\\r?\n$", source):
            # Phase-2 splicing can change comments, tokens and scope before lexing.
            # Only the known directive continuation grammar is admitted.
            if not directive or self.comment or self.raw_end or self.quote or "//" in source or "/*" in source:
                raise DerivationError("unsupported physical line splice")
        tokens = []
        index = 0
        while index < len(source):
            if self.comment:
                end = source.find("*/", index)
                if end < 0:
                    return tokens, directive
                self.comment = False
                index = end + 2
                continue
            if self.raw_end:
                end = source.find(self.raw_end, index)
                if end < 0:
                    return tokens, directive
                index = end + len(self.raw_end)
                self.raw_end = None
                continue
            if source[index].isspace():
                index += 1
                continue
            if source.startswith("//", index):
                break
            if source.startswith("/*", index):
                self.comment = True
                index += 2
                continue
            raw = RAW_START.match(source, index)
            if raw:
                start = index
                ending = ")" + raw.group(1) + '"'
                end = source.find(ending, raw.end())
                if end < 0:
                    self.raw_end = ending
                    return tokens + [Token(source[start:], start, len(source), True)], directive
                index = end + len(ending)
                tokens.append(Token(source[start:index], start, index, True))
                continue
            if self.quote or source[index] in "\"'":
                start = index
                quote = self.quote or source[index]
                index += not bool(self.quote)
                self.quote = None
                while index < len(source):
                    if source[index] == "\\":
                        index += 2
                        continue
                    if source[index] == quote:
                        index += 1
                        break
                    if source[index] == "\n":
                        raise DerivationError("unterminated literal")
                    index += 1
                else:
                    if source.rstrip().endswith("\\"):
                        self.quote = quote
                    else:
                        raise DerivationError("unterminated literal")
                tokens.append(Token(source[start:index], start, index, True))
                continue
            match = IDENT.match(source, index) or NUMBER.match(source, index)
            if match:
                tokens.append(Token(match.group(), index, match.end()))
                index = match.end()
                continue
            operator = next((op for op in OPERATORS if source.startswith(op,index)), None)
            end = index + len(operator or source[index])
            tokens.append(Token(source[index:end],index,end))
            index = end
        directive = directive or bool(tokens and tokens[0].text == "#")
        self.directive_continuation = directive and source.rstrip().endswith("\\")
        return tokens, directive

    def finish(self):
        if self.comment or self.raw_end or self.quote or self.directive_continuation:
            raise DerivationError("unterminated lexical region")

def lex(source):
    lexer = Lexer()
    tokens, directive = lexer.line(source)
    lexer.finish()
    if directive:
        raise DerivationError("directive inside instruction template")
    return tokens

def spelling(tokens):
    return [token.text for token in tokens]

def values(source):
    return spelling(lex(source))

def require(tokens, expected):
    if spelling(tokens) != values(expected):
        raise DerivationError("unsupported instruction shape: " + " ".join(spelling(tokens))[:180])

def matching(tokens, opening):
    pairs = {"(":")", "[":"]", "{":"}"}
    stack = []
    for index in range(opening,len(tokens)):
        token = tokens[index]
        if token.literal:
            continue
        if token.text in pairs:
            stack.append(pairs[token.text])
        elif token.text in pairs.values():
            if not stack or stack.pop() != token.text:
                raise DerivationError("unbalanced expression")
            if not stack:
                return index
    raise DerivationError("unterminated expression")

def call_arguments(tokens, name):
    if name + 1 >= len(tokens) or tokens[name+1].text != "(":
        raise DerivationError("memory helper is not a direct call")
    end = matching(tokens,name+1)
    args, start, index = [], name+2, name+2
    while index < end:
        if tokens[index].text in ("(","[","{"):
            index = matching(tokens,index)+1
            continue
        if tokens[index].text == ",":
            args.append(tokens[start:index])
            start = index+1
        index += 1
    if start < end:
        args.append(tokens[start:end])
    return args,end

def excerpt(source,tokens):
    if not tokens:
        raise DerivationError("empty expression")
    return source[tokens[0].start:tokens[-1].end]

def statement_end(tokens,start):
    index = start
    while index < len(tokens):
        if tokens[index].text in ("(","[","{"):
            index = matching(tokens,index)+1
            continue
        if tokens[index].text == ";":
            return index
        index += 1
    raise DerivationError("missing statement")

def ea_prefix(tokens):
    require(tokens[:4],"{ uint64_t ea =")
    end = statement_end(tokens,4)
    if end == 4 or matching(tokens,0) != len(tokens)-1:
        raise DerivationError("unsupported instruction block")
    return end

def register_address(tokens):
    if len(tokens) != 7 or spelling(tokens[:5]) != ["&","ctx","->","vr","["] or tokens[-1].text != "]":
        raise DerivationError("unsupported vector buffer")
    try:
        number = int(tokens[5].text,0)
    except ValueError as error:
        raise DerivationError("nonconstant vector index") from error
    if not 0 <= number < 32:
        raise DerivationError("vector index out of range")

def replace_local(source,changes):
    for start,end,replacement in sorted(changes,reverse=True):
        source = source[:start]+replacement+source[end:]
    return source

def reservation_block(source,tokens):
    end_ea = ea_prefix(tokens)
    sc = [i for i,t in enumerate(tokens) if t.text in ("ppu_stwcx32","ppu_stdcx64")]
    if sc:
        if len(sc) != 1:
            raise DerivationError("multiple SCs in one block")
        index = sc[0]
        require(tokens[end_ea+1:index],"int _sc = (ctx->reserve_addr == (uint32_t)ea) &&")
        args,end = call_arguments(tokens,index)
        if len(args) != 3:
            raise DerivationError("SC arity")
        width = 32 if tokens[index].text == "ppu_stwcx32" else 64
        require(args[0],"ea")
        require(args[1],"(uint32_t)ctx->reserve_value" if width == 32 else "ctx->reserve_value")
        require(tokens[end+1:],"; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (_sc ? (2u << 28) : 0u); ctx->reserve_addr = 0; }")
        replacement = f"ppu_native_store_conditional{width}(ctx, ea, {excerpt(source,args[2])})"
        return replace_local(source,[(tokens[end_ea+4].start,tokens[end].end,replacement)]),f"sc{width}"
    index = next((i for i,t in enumerate(tokens) if t.text in ("vm_read32","vm_read64")),None)
    if index is None:
        raise DerivationError("reserve mirrors without LR")
    before = tokens[end_ea+1:index]
    if len(before) != 7 or spelling(before[:4]) != ["ctx","->","gpr","["] or spelling(before[-2:]) != ["]","="]:
        raise DerivationError("unsupported LR destination")
    register = before[4].text
    if not register.isdigit() or not 0 <= int(register) < 32:
        raise DerivationError("LR register")
    args,end = call_arguments(tokens,index)
    if len(args) != 1:
        raise DerivationError("LR arity")
    require(args[0],"ea")
    require(tokens[end+1:],f"; ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[{register}]; }}")
    width = tokens[index].text.removeprefix("vm_read")
    return replace_local(source,[(tokens[index].start,tokens[index+1].end,f"ppu_native_load_reserve{width}(ctx, ")]),f"lr{width}"


def raw_block(source,tokens):
    end_ea = ea_prefix(tokens)
    names = spelling(tokens)
    if "m" in names and "for" in names:
        tail = tokens[end_ea+1:]
        prefix = values("uint32_t sh = (uint32_t)(ea & 0xF); uint8_t* m = vm_base + (uint32_t)(ea & ~0xFULL); uint8_t* d = (uint8_t*)")
        if spelling(tail[:len(prefix)]) != prefix:
            raise DerivationError("unsupported raw alias prefix")
        start = len(prefix)
        register_address(tail[start:start+7])
        destination = excerpt(source,tail[start:start+7])
        require(tail[start+7:],"; for (int i = 0; i < 16; i++) d[i] = (sh + (uint32_t)i < 16) ? m[sh + i] : 0; }")
        replacement = excerpt(source,tokens[:end_ea+1])+" uint32_t sh = (uint32_t)(ea & 0xF); unsigned char _ppu_memory_bytes[16] = {0}; "
        replacement += f"ppu_native_read_bytes(ctx, (uint32_t)ea, _ppu_memory_bytes, 16 - sh); memcpy({destination}, _ppu_memory_bytes, 16); }}"
        return replace_local(source,[(tokens[0].start,tokens[-1].end,replacement)]),"raw_left_load"
    calls = [i for i,t in enumerate(tokens) if t.text == "memcpy"]
    if len(calls) != 1:
        raise DerivationError("unsupported raw memory statement")
    index = calls[0]
    args,end = call_arguments(tokens,index)
    if len(args) != 3:
        raise DerivationError("raw memcpy arity")
    guests = [i for i,arg in enumerate(args[:2]) if "vm_base" in spelling(arg)]
    if len(guests) != 1:
        raise DerivationError("raw copy needs exactly one guest operand")
    guest = guests[0]
    require(args[guest],"vm_base + (uint32_t)ea")
    try:
        length = int(excerpt(source,args[2]),0)
    except ValueError as error:
        raise DerivationError("nonconstant transfer length") from error
    if length not in (1,2,4,8,16):
        raise DerivationError("unsupported transfer length")
    other = args[1-guest]
    if "memset" in names:
        if guest != 1 or length not in (1,2,4):
            raise DerivationError("unsupported element transfer")
        align_end = statement_end(tokens,end_ea+1)
        require(tokens[end_ea+1:align_end+1],f"ea &= ~{length-1}ULL;")
        zero_index = align_end+1
        if tokens[zero_index].text != "memset":
            raise DerivationError("element zero prefix")
        zero_args,zero_end = call_arguments(tokens,zero_index)
        if len(zero_args) != 3:
            raise DerivationError("element zero arity")
        register_address(zero_args[0])
        require(zero_args[1],"0")
        require(zero_args[2],"16")
        require(tokens[zero_end+1:index],";")
        destination = excerpt(source,zero_args[0])
        require(other,f"(uint8_t*){destination} + (ea & 15)")
        require(tokens[end+1:],"; }")
        replacement = excerpt(source,tokens[:align_end+1])+" unsigned char _ppu_memory_bytes[16] = {0}; "
        replacement += f"ppu_native_read_bytes(ctx, (uint32_t)ea, _ppu_memory_bytes + (ea & 15), {length}); memcpy({destination}, _ppu_memory_bytes, 16); }}"
        return replace_local(source,[(tokens[0].start,tokens[-1].end,replacement)]),"raw_element_load"
    if length == 16:
        register_address(other)
        require(tokens[end_ea+1:index],"")
        require(tokens[end+1:],"; }")
        kind = "raw_vector_write" if guest == 0 else "raw_vector_read"
    else:
        if guest != 1:
            raise DerivationError("unobserved byte-reverse store")
        require(tokens[end_ea+1:index],f"uint{length*8}_t raw;")
        require(other,"&raw")
        suffix = tokens[end+1:]
        if len(suffix) != 11 or spelling(suffix[:5]) != [";","ctx","->","gpr","["] or spelling(suffix[-5:]) != ["]","=","raw",";","}"]:
            raise DerivationError("unsupported reverse-read publication")
        kind = "raw_reverse_read"
    operation = "write" if guest == 0 else "read"
    replacement = f"ppu_native_{operation}_bytes(ctx, (uint32_t)ea, {excerpt(source,other)}, {length})"
    return replace_local(source,[(tokens[index].start,tokens[end].end,replacement)]),kind


def regular_calls(source,tokens):
    changes = []
    for index,token in enumerate(tokens):
        if token.literal or token.text not in LEGACY:
            continue
        name = token.text
        args,end = call_arguments(tokens,index)
        if name.startswith("vm_read"):
            arity,target = 1,"ppu_native_read"+name[7:]
        elif name.startswith("vm_write"):
            arity,target = 2,"ppu_native_write"+name[8:]
        elif name == "ppu_atomic_add16":
            arity,target = 2,"ppu_native_atomic_add16"
        elif name == "ppu_dcbz128":
            arity,target = 1,"ppu_native_zero128"
        elif name == "ppu_stvx16":
            if len(args) != 2:
                raise DerivationError("stvx arity")
            register_address(args[1])
            replacement = f"ppu_native_write_bytes(ctx, {excerpt(source,args[0])}, {excerpt(source,args[1])}, 16)"
            changes.append((token.start,tokens[end].end,replacement))
            continue
        else:
            raise DerivationError("unhandled helper "+name)
        if len(args) != arity:
            raise DerivationError("helper arity "+name)
        changes.append((token.start,tokens[index+1].end,target+"(ctx, "))
    return changes


def sha(data):
    return hashlib.sha256(data).hexdigest()


def apply_edits(source,changes):
    original = source.encode("utf-8")
    output,records,cursor,new_cursor = [],[],0,0
    for start,end,replacement,kind in sorted(changes):
        if start < cursor or end < start or end > len(original):
            raise DerivationError("overlapping/out-of-range edits")
        untouched = original[cursor:start]
        encoded = replacement.encode("utf-8")
        output.extend((untouched,encoded))
        new_cursor += len(untouched)
        records.append([start,end-start,new_cursor,len(encoded),kind,sha(original[start:end]),sha(encoded)])
        new_cursor += len(encoded)
        cursor = end
    output.append(original[cursor:])
    return b"".join(output).decode("utf-8"),records


def verify_edits(original,output,records):
    old_cursor = new_cursor = 0
    for start,old_size,new_start,new_size,kind,old_hash,new_hash in records:
        if not kind or start < old_cursor or new_start < new_cursor or old_size < 0 or new_size < 0:
            raise DerivationError("overlapping/invalid edit coverage")
        if start+old_size > len(original) or new_start+new_size > len(output):
            raise DerivationError("edit outside file")
        if original[old_cursor:start] != output[new_cursor:new_start]:
            raise DerivationError("unreported unchanged-span edit")
        if sha(original[start:start+old_size]) != old_hash or sha(output[new_start:new_start+new_size]) != new_hash:
            raise DerivationError("edit hash mismatch")
        old_cursor,new_cursor = start+old_size,new_start+new_size
    if original[old_cursor:] != output[new_cursor:]:
        raise DerivationError("unreported trailing edit")


def validate_owned_body(tokens,opening,in_function):
    names = spelling(tokens)
    for index,token in enumerate(tokens):
        if token.literal:
            continue
        if token.text.startswith("ppu_native_"):
            raise DerivationError("input is already contextual; fixture is not an original")
        if in_function and token.text == "ctx" and index+1 < len(tokens) and tokens[index+1].text in ("=","+=","-=","++","--"):
            raise DerivationError("execution context rebinding")
        if in_function and token.text.startswith("func_") and index+1 < len(tokens) and tokens[index+1].text == "(" and not opening:
            args,_ = call_arguments(tokens,index)
            if len(args) != 1 or spelling(args[0]) != ["ctx"]:
                raise DerivationError("generated call does not preserve execution owner")
        if token.text == "reinterpret_cast" and "gpr" in names:
            raise DerivationError("guest-register pointer cast")
        if token.text != "(":
            continue
        cursor = index+1
        qualifiers = {"const","volatile","unsigned","signed","long","short"}
        scalar_types = {"uint8_t","uint16_t","uint32_t","uint64_t","int8_t","int16_t","int32_t","int64_t","void","char","float","double","int"}
        while cursor < len(tokens) and tokens[cursor].text in qualifiers:
            cursor += 1
        if cursor >= len(tokens) or tokens[cursor].text not in scalar_types:
            continue
        cursor += 1
        pointer = False
        while cursor < len(tokens) and tokens[cursor].text in qualifiers|{"*"}:
            pointer = pointer or tokens[cursor].text == "*"
            cursor += 1
        if not pointer or cursor >= len(tokens) or tokens[cursor].text != ")":
            continue
        end = cursor
        stop = next((i for i in range(end+1,len(tokens)) if tokens[i].text == ";"),len(tokens))
        operand = names[end+1:stop]
        if not operand or operand[0] == "&":
            continue
        if operand[0] == "ea" or operand[0][0].isdigit() or any(operand[i:i+4] == ["ctx","->","gpr","["] for i in range(len(operand)-3)):
            raise DerivationError("unreviewed guest address to host pointer")


def function_coverage(names):
    signatures = "".join("void " + name + "(ppu_context* ctx) {\n" for name in names)
    digest = sha(signatures.encode())
    return {"count":len(names), "names":names,
            "original_signature_sha256":digest, "derived_signature_sha256":digest}


def derive_unit(source,unit_name,coverage_names=None):
    lexer,changes,counts = Lexer(),[],Counter({key:0 for key in PATTERNS})
    depth,function_depth,current_function,byte_offset = 0,None,None,0
    for number,line in enumerate(source.splitlines(keepends=True),1):
        try:
            tokens,directive = lexer.line(line)
            identifiers = {t.text for t in tokens if not t.literal}
            memory,reserve = identifiers & LEGACY,identifiers & RESERVES
            opening = len(tokens) == 8 and spelling(tokens[:1]) == ["void"] and tokens[1].text.startswith("func_")
            if opening:
                require(tokens[2:],"(ppu_context* ctx) {")
                suffix = tokens[1].text.removeprefix("func_")
                if len(suffix) != 8 or any(ch not in "0123456789abcdefABCDEF" for ch in suffix):
                    raise DerivationError("function identity")
                if current_function is not None:
                    raise DerivationError("nested generated function")
                function_depth,current_function = depth+1,tokens[1].text
                if coverage_names is not None:
                    coverage_names.append(current_function)
            if directive:
                if memory or reserve:
                    raise DerivationError("memory in preprocessor directive")
                byte_offset += len(line.encode("utf-8"))
                continue
            validate_owned_body(tokens,opening,current_function is not None)
            if memory or reserve:
                for key,pattern in PATTERNS.items():
                    if pattern.search(line):
                        counts[key] += 1
            local = []
            if memory or reserve:
                if current_function is None:
                    require(tokens,'extern "C" uint8_t* vm_base;')
                    local = [(tokens[0].start,tokens[-1].end,"","base_declaration")]
                elif reserve:
                    replacement,kind = reservation_block(line,tokens)
                    local = [(0,len(line.rstrip("\r\n")),replacement.rstrip("\r\n"),kind)]
                elif "vm_base" in identifiers:
                    replacement,kind = raw_block(line,tokens)
                    local = [(0,len(line.rstrip("\r\n")),replacement.rstrip("\r\n"),kind)]
                else:
                    local = [(a,b,value,"context_call") for a,b,value in regular_calls(line,tokens)]
            if current_function and "ppu_context" in identifiers and not opening:
                raise DerivationError("standalone/nested CPU declaration")
            for start,end,replacement,kind in local:
                counts["edit_"+kind] += 1
                changes.append((byte_offset+len(line[:start].encode()),byte_offset+len(line[:end].encode()),replacement,kind))
            for token in tokens:
                if token.literal:
                    continue
                if token.text == "{":
                    depth += 1
                elif token.text == "}":
                    depth -= 1
                    if depth < 0:
                        raise DerivationError("unmatched closing brace")
                    if function_depth is not None and depth < function_depth:
                        current_function = function_depth = None
            byte_offset += len(line.encode("utf-8"))
        except DerivationError as error:
            raise DerivationError(f"{unit_name}:{number}: {error}") from error
    lexer.finish()
    if depth or current_function:
        raise DerivationError(unit_name+": unclosed scope")
    output,records = apply_edits(source,changes)
    verify_edits(source.encode(),output.encode(),records)
    audit_unit(output,unit_name)
    return output,records,dict(counts)


def audit_unit(source,unit_name):
    lexer = Lexer()
    for number,line in enumerate(source.splitlines(keepends=True),1):
        tokens,_ = lexer.line(line)
        for token in tokens:
            if not token.literal and (token.text in LEGACY or token.text.startswith(("ppu__ld","ppu__st","ppu_slow_","__atomic_","__sync_"))):
                raise DerivationError(f"{unit_name}:{number}: forbidden residual {token.text}")
        for index,token in enumerate(tokens):
            if token.literal or not token.text.startswith("ppu_native_"):
                continue
            args,_ = call_arguments(tokens,index)
            if not args or spelling(args[0]) != ["ctx"]:
                raise DerivationError("native helper missing explicit ctx")
    lexer.finish()


def cpu_region(source):
    lexer,tokens,offset = Lexer(),[],0
    for line in source.splitlines(keepends=True):
        part,directive = lexer.line(line)
        if not directive:
            tokens.extend(Token(t.text,t.start+offset,t.end+offset,t.literal) for t in part)
        offset += len(line)
        if any(t.text == "ppu_context" for t in part) and "}" in spelling(part):
            break
    names = spelling(tokens)
    for index in range(len(tokens)-3):
        if names[index:index+4] == ["typedef","struct","ppu_context","{"]:
            end = matching(tokens,index+3)
            require(tokens[end+1:end+3],"ppu_context;")
            return source[tokens[index].start:tokens[end+2].end].encode()
    raise DerivationError("missing tagged CPU type")


def derive_header(source):
    begin,end = "/* Memory access helpers (provided by runtime) */","/* Syscall handler */"
    if source.count(begin) != 1 or source.count(end) != 1:
        raise DerivationError("header landmarks changed")
    start,stop = source.index(begin),source.index(end)
    if start >= stop:
        raise DerivationError("header landmark order")
    replacement = '/* Explicit native memory helpers; legacy fast paths are absent. */\n#include "native_memory_abi.h"\n\n'
    output,edits = apply_edits(source,[(len(source[:start].encode()),len(source[:stop].encode()),replacement,"header_memory_region")])
    old_cpu,new_cpu = sha(cpu_region(source)),sha(cpu_region(output))
    if old_cpu != new_cpu:
        raise DerivationError("CPU type bytes changed")
    verify_edits(source.encode(),output.encode(),edits)
    audit_unit(output,"ppu_recomp.h")
    return output,edits,old_cpu,new_cpu

def atomic_publish(stage,output):
    # Linux renameat2 is the atomic no-replace boundary. Plain rename can replace
    # a concurrently created empty directory; do not weaken this on other hosts.
    import ctypes
    import errno
    import os
    library = ctypes.CDLL(None,use_errno=True)
    operation = getattr(library,"renameat2",None)
    if operation is None:
        raise DerivationError("atomic no-replace directory publication unavailable")
    operation.argtypes = [ctypes.c_int,ctypes.c_char_p,ctypes.c_int,ctypes.c_char_p,ctypes.c_uint]
    operation.restype = ctypes.c_int
    AT_FDCWD,RENAME_NOREPLACE = -100,1
    if operation(AT_FDCWD,os.fsencode(stage),AT_FDCWD,os.fsencode(output),RENAME_NOREPLACE):
        status = ctypes.get_errno()
        if status == errno.EEXIST:
            raise DerivationError("output appeared during atomic publication")
        raise DerivationError("atomic publication failed errno="+str(status))


def _publish(inputs,header,output,scope,project_root):
    output = Path(output)
    if output.exists():
        raise DerivationError("output already exists")
    abi = (project_root/ABI_PATH).read_bytes()
    if sha(abi) != ABI_SHA256:
        raise DerivationError("ABI contract hash mismatch")
    output.parent.mkdir(parents=True,exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix="."+output.name+".pending-",dir=output.parent))
    try:
        manifest = {"schema":1,"scope":scope,"original_inputs":{},"outputs":{},
                    "abi_sha256":ABI_SHA256,"tool_sha256":sha(Path(__file__).read_bytes()),
                    "edit_fields":["original_start","original_length","derived_start","derived_length","class","original_sha256","derived_sha256"]}
        original = header.read_bytes()
        if scope == "production" and sha(original) != PINS["ppu_recomp.h"]:
            raise DerivationError("header changed after pin verification")
        transformed,edits,old_cpu,new_cpu = derive_header(original.decode("utf-8"))
        manifest["cpu_type_original_sha256"] = old_cpu
        manifest["cpu_type_derived_sha256"] = new_cpu
        def write_result(name,source,derived,records,counts,coverage=None):
            encoded = derived.encode("utf-8")
            (stage/name).write_bytes(encoded)
            manifest["original_inputs"][name] = {"sha256":sha(source),"bytes":len(source)}
            manifest["outputs"][name] = {"sha256":sha(encoded),"bytes":len(encoded),"counts":counts,"edits":records}
            if coverage is not None:
                manifest["outputs"][name]["function_coverage"] = function_coverage(coverage)
        write_result("ppu_recomp.h",original,transformed,edits,{})
        del original,transformed,edits
        for path in inputs:
            original = path.read_bytes()
            if scope == "production" and sha(original) != PINS[path.name]:
                raise DerivationError("input changed after pin verification "+path.name)
            print("DERIVATION_UNIT_BEGIN "+path.name,flush=True)
            coverage = []
            transformed,edits,counts = derive_unit(original.decode("utf-8"),path.name,coverage)
            if scope == "production":
                expected = dict(zip(PATTERNS,EXPECTED[path.stem[-3:]]))
                if any(counts[key] != value for key,value in expected.items()):
                    raise DerivationError("census mismatch "+path.name+" "+json.dumps(counts,sort_keys=True))
            write_result(path.name,original,transformed,edits,counts,coverage)
            print("DERIVATION_UNIT_PASS "+path.name+" edits="+str(len(edits)),flush=True)
            del original,transformed,edits,counts
        for path in [header,*inputs]:
            key = "ppu_recomp.h" if path == header else path.name
            if sha(path.read_bytes()) != manifest["original_inputs"][key]["sha256"]:
                raise DerivationError("input changed during derivation "+path.name)
        (stage/"native_memory_abi.h").write_bytes(abi)
        manifest["outputs"]["native_memory_abi.h"] = {"sha256":sha(abi),"bytes":len(abi),"counts":{},"edits":[]}
        with (stage/"manifest.json").open("w") as stream:
            json.dump(manifest,stream,sort_keys=True,separators=(",",":"))
            stream.write("\n")
        if output.exists():
            raise DerivationError("output appeared during derivation")
        atomic_publish(stage,output)
        return manifest
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def derive_fixture(source,header,output,project_root=ROOT):
    source = Path(source)
    if source.name in ("ppu_recomp.h","native_memory_abi.h","manifest.json"):
        raise DerivationError("reserved fixture name")
    return _publish([source],Path(header),Path(output),"fixture_only",Path(project_root))


def derive_project(project_root,output):
    project_root = Path(project_root)
    folder = project_root/SOURCE_PREFIX
    for name,expected in PINS.items():
        if sha((folder/name).read_bytes()) != expected:
            raise DerivationError("pinned input hash mismatch "+name)
    return _publish([folder/name for name in PINS if name.endswith(".cpp")],folder/"ppu_recomp.h",Path(output),"production",project_root)


def apply_process_limits(seconds,memory_mib,cpus):
    import os
    import resource
    import signal
    if seconds <= 0 or memory_mib <= 0 or cpus <= 0:
        raise DerivationError("process limits must be positive")
    resource.setrlimit(resource.RLIMIT_AS,(memory_mib*1024*1024,memory_mib*1024*1024))
    resource.setrlimit(resource.RLIMIT_CPU,(seconds,seconds))
    available = sorted(os.sched_getaffinity(0))
    os.sched_setaffinity(0,available[:cpus])
    def expired(_signal,_frame):
        raise DerivationError("owned derivation deadline expired")
    signal.signal(signal.SIGALRM,expired)
    signal.alarm(seconds)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-root",type=Path,default=ROOT)
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--fixture",type=Path)
    parser.add_argument("--header",type=Path)
    parser.add_argument("--max-seconds",type=int,default=900)
    parser.add_argument("--max-memory-mib",type=int,default=4096)
    parser.add_argument("--max-cpus",type=int,default=2)
    args = parser.parse_args()
    if bool(args.fixture) != bool(args.header):
        parser.error("--fixture and --header must be supplied together")
    try:
        apply_process_limits(args.max_seconds,args.max_memory_mib,args.max_cpus)
        manifest = derive_fixture(args.fixture,args.header,args.output,args.project_root) if args.fixture else derive_project(args.project_root,args.output)
    except (DerivationError,OSError,UnicodeError,MemoryError) as error:
        parser.exit(1,f"DERIVATION_FAIL {error}\n")
    print("DERIVATION_PASS scope="+manifest["scope"]+" outputs="+str(len(manifest["outputs"])))

if __name__ == "__main__":
    main()
