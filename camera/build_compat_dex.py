#!/usr/bin/env python3
"""
Zero-JDK pure-Python DEX 035 compiler that builds `classes15.dex` containing
the 14 minimal Vivo framework/SDK compatibility classes required by OriginOS 4's
VivoCamera.apk (com.android.camera) on AOSP Android 17 GSI.

Classes emitted:
  - android.os.FtBuild
  - android.media.AudioFeatures (+ $AudioFeatureCallback, $TagParameters)
  - android.preference.VivoCheckBoxPreference (extends CheckBoxPreference)
  - android.widget.BbkMoveBoolButton (extends ImageView) + $OnCheckedChangeListener
  - com.vivo.app.VivoPreferenceActivity (extends PreferenceActivity)
  - com.vivo.common.BbkTitleView (extends RelativeLayout, TITLE_BTN_* constants)
  - com.vivo.framework.themeicon.ThemeIconManager (singleton)
  - com.vivo.framework.touchscreen.TouchScreenManager (singleton)
  - vivo.app.themeicon.{SystemColorListener, SystemColorWheelListener, SystemFilletListener}
"""
import hashlib
import os
import struct
import zlib

ACC_PUBLIC = 0x0001
ACC_STATIC = 0x0008
ACC_FINAL = 0x0010
ACC_INTERFACE = 0x0200
ACC_ABSTRACT = 0x0400
ACC_CONSTRUCTOR = 0x10000


def uleb128(val: int) -> bytes:
    out = bytearray()
    while True:
        b = val & 0x7F
        val >>= 7
        if val:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def sleb128(val: int) -> bytes:
    out = bytearray()
    while True:
        b = val & 0x7F
        val >>= 7
        if (val == 0 and not (b & 0x40)) or (val == -1 and (b & 0x40)):
            out.append(b)
            return bytes(out)
        out.append(b | 0x80)


def enc_35c(op: int, mid: int, rlist: list[int]) -> list[int]:
    a = len(rlist)
    assert 0 <= a <= 5 and all(0 <= r <= 15 for r in rlist)
    c = rlist[0] if a > 0 else 0
    d = rlist[1] if a > 1 else 0
    e = rlist[2] if a > 2 else 0
    f = rlist[3] if a > 3 else 0
    g = rlist[4] if a > 4 else 0
    return [(a << 12) | (g << 8) | op, mid, (f << 12) | (e << 8) | (d << 4) | c]


def align4(buf: bytearray):
    rem = len(buf) % 4
    if rem:
        buf.extend(b"\x00" * (4 - rem))


def shorty_of(ret_t: str, param_ts: list[str]) -> str:
    def s(t: str) -> str:
        return "L" if t[0] in ("L", "[") else t[0]
    return s(ret_t) + "".join(s(p) for p in param_ts)


class DexBuilder:
    def __init__(self):
        self.strings: set[str] = set()
        self.types: set[str] = set()
        self.protos: set[tuple[str, tuple[str, ...]]] = set()
        self.fields: set[tuple[str, str, str]] = set()   # (cls, type, name)
        self.methods: set[tuple[str, str, str, tuple[str, ...]]] = set()  # (cls, name, ret, params)
        self.classes: list[dict] = []

    def add_string(self, s: str):
        self.strings.add(s)

    def add_type(self, t: str):
        self.types.add(t)
        self.strings.add(t)

    def add_proto(self, ret_t: str, param_ts: tuple[str, ...]):
        self.add_type(ret_t)
        for p in param_ts:
            self.add_type(p)
        self.add_string(shorty_of(ret_t, list(param_ts)))
        self.protos.add((ret_t, param_ts))

    def add_field_ref(self, cls_t: str, fld_t: str, name: str):
        self.add_type(cls_t)
        self.add_type(fld_t)
        self.add_string(name)
        self.fields.add((cls_t, fld_t, name))

    def add_method_ref(self, cls_t: str, name: str, ret_t: str, param_ts: tuple[str, ...]):
        self.add_type(cls_t)
        self.add_string(name)
        self.add_proto(ret_t, param_ts)
        self.methods.add((cls_t, name, ret_t, param_ts))

    def add_class(self, cls_t: str, super_t: str, access: int,
                  interfaces=None, static_fields=None, instance_fields=None,
                  direct_methods=None, virtual_methods=None):
        self.add_type(cls_t)
        self.add_type(super_t)
        ifaces = list(interfaces or [])
        sfs = static_fields or []
        ifs = instance_fields or []
        dms = direct_methods or []
        vms = virtual_methods or []
        for it in ifaces:
            self.add_type(it)
        for fld_t, name, acc, init_val in sfs:
            self.add_field_ref(cls_t, fld_t, name)
        for fld_t, name, acc in ifs:
            self.add_field_ref(cls_t, fld_t, name)
        for m in dms + vms:
            self.add_method_ref(cls_t, m["name"], m["ret"], tuple(m["params"]))
            if m.get("body_fn"):
                # Pre-register any referenced strings/types/fields/methods via dry callback
                m["pre_reg"](self)
        self.classes.append({
            "cls": cls_t,
            "super": super_t,
            "access": access,
            "interfaces": ifaces,
            "static_fields": sfs,
            "instance_fields": ifs,
            "direct_methods": dms,
            "virtual_methods": vms,
        })

    def build(self) -> bytes:
        # 1. Sort all ID pools strictly according to DEX specification
        str_list = sorted(self.strings, key=lambda s: s.encode("utf-8"))
        str_idx = {s: i for i, s in enumerate(str_list)}

        type_list = sorted(self.types, key=lambda t: str_idx[t])
        type_idx = {t: i for i, t in enumerate(type_list)}

        proto_list = sorted(
            self.protos,
            key=lambda p: (type_idx[p[0]], [type_idx[x] for x in p[1]])
        )
        proto_idx = {p: i for i, p in enumerate(proto_list)}

        field_list = sorted(
            self.fields,
            key=lambda f: (type_idx[f[0]], str_idx[f[2]], type_idx[f[1]])
        )
        field_idx = {f: i for i, f in enumerate(field_list)}

        method_list = sorted(
            self.methods,
            key=lambda m: (type_idx[m[0]], str_idx[m[1]], proto_idx[(m[2], m[3])])
        )
        method_idx = {m: i for i, m in enumerate(method_list)}

        # Topological order: any superclass defined in this DEX MUST precede its subclass!
        by_cls = {c["cls"]: c for c in self.classes}
        cls_list = []
        visited = set()

        def visit_cls(name):
            if name in visited or name not in by_cls:
                return
            c = by_cls[name]
            visit_cls(c["super"])
            visited.add(name)
            cls_list.append(c)

        for c in sorted(self.classes, key=lambda x: type_idx[x["cls"]]):
            visit_cls(c["cls"])

        ctx = {
            "str": str_idx,
            "type": type_idx,
            "proto": proto_idx,
            "field": field_idx,
            "method": method_idx,
        }

        # Fixed header & ID tables layout
        header_size = 0x70
        str_ids_off = header_size
        type_ids_off = str_ids_off + len(str_list) * 4
        proto_ids_off = type_ids_off + len(type_list) * 4
        field_ids_off = proto_ids_off + len(proto_list) * 12
        method_ids_off = field_ids_off + len(field_list) * 8
        class_defs_off = method_ids_off + len(method_list) * 8
        data_off = class_defs_off + len(cls_list) * 32

        data = bytearray()

        # Helper to track map items
        map_items = [
            (0x0000, 1, 0),
            (0x0001, len(str_list), str_ids_off),
            (0x0002, len(type_list), type_ids_off),
            (0x0003, len(proto_list), proto_ids_off),
        ]
        if field_list:
            map_items.append((0x0004, len(field_list), field_ids_off))
        if method_list:
            map_items.append((0x0005, len(method_list), method_ids_off))
        map_items.append((0x0006, len(cls_list), class_defs_off))

        # Section: type_lists (for protos with non-empty params and class interfaces)
        align4(data)
        type_list_offsets = {}
        iface_offsets = {}
        tl_start = data_off + len(data)
        tl_count = 0
        for p in proto_list:
            params = p[1]
            if not params:
                type_list_offsets[p] = 0
                continue
            align4(data)
            type_list_offsets[p] = data_off + len(data)
            data.extend(struct.pack("<I", len(params)))
            for pt in params:
                data.extend(struct.pack("<H", type_idx[pt]))
            tl_count += 1
        for c in cls_list:
            ifaces = c["interfaces"]
            if not ifaces:
                iface_offsets[c["cls"]] = 0
                continue
            align4(data)
            iface_offsets[c["cls"]] = data_off + len(data)
            data.extend(struct.pack("<I", len(ifaces)))
            for it in ifaces:
                data.extend(struct.pack("<H", type_idx[it]))
            tl_count += 1
        if tl_count:
            map_items.append((0x1001, tl_count, tl_start))

        # Section: code_items
        align4(data)
        code_start = data_off + len(data)
        code_count = 0
        code_offsets = {}
        for c in cls_list:
            for m in c["direct_methods"] + c["virtual_methods"]:
                if m.get("body_fn") is None:
                    continue
                align4(data)
                key = (c["cls"], m["name"], m["ret"], tuple(m["params"]))
                code_offsets[key] = data_off + len(data)
                res = m["body_fn"](ctx)
                if len(res) == 4:
                    regs, ins, outs, insns = res
                    tries = []
                else:
                    regs, ins, outs, insns, tries = res
                data.extend(struct.pack("<HHHHII", regs, ins, outs, len(tries), 0, len(insns)))
                data.extend(struct.pack(f"<{len(insns)}H", *insns))
                if tries:
                    if len(insns) % 2 == 1:
                        data.extend(b"\x00\x00")
                    # Build encoded_catch_handler_list (each try has a single catch-all handler)
                    handler_list = bytearray(uleb128(len(tries)))
                    handler_offs = []
                    for _start, _cnt, catch_all in tries:
                        handler_offs.append(len(handler_list))
                        handler_list.extend(sleb128(0))  # size=0 -> catch-all only
                        handler_list.extend(uleb128(catch_all))
                    for (start_u, cnt_u, _), hoff in zip(tries, handler_offs):
                        data.extend(struct.pack("<IHH", start_u, cnt_u, hoff))
                    data.extend(handler_list)
                code_count += 1
        if code_count:
            map_items.append((0x2001, code_count, code_start))

        # Section: string_data_items
        str_data_start = data_off + len(data)
        str_data_offsets = []
        for s in str_list:
            str_data_offsets.append(data_off + len(data))
            utf8 = s.encode("utf-8")
            data.extend(uleb128(len(s)))
            data.extend(utf8)
            data.append(0)
        map_items.append((0x2002, len(str_list), str_data_start))

        # Section: encoded_array_items (static_values for classes with non-default initializers)
        sval_start = data_off + len(data)
        sval_count = 0
        sval_offsets = {}
        for c in cls_list:
            sfs = sorted(c["static_fields"], key=lambda x: field_idx[(c["cls"], x[0], x[1])])
            vals = [x[3] for x in sfs]
            if any(v is not None for v in vals):
                sval_offsets[c["cls"]] = data_off + len(data)
                data.extend(uleb128(len(vals)))
                for fld_t, _, _, v in sfs:
                    if v is None:
                        if fld_t in ("I", "Z", "F"):
                            data.append(0x04)  # VALUE_INT, size 1
                            data.append(0)
                        else:
                            data.append(0x1E)  # VALUE_NULL
                    elif fld_t == "I":
                        # Encode signed int in minimal bytes
                        b = v.to_bytes(4, "little", signed=True)
                        # Trim redundant sign-extension bytes
                        sz = 4
                        while sz > 1 and ((b[sz - 1] == 0x00 and not (b[sz - 2] & 0x80)) or (b[sz - 1] == 0xFF and (b[sz - 2] & 0x80))):
                            sz -= 1
                        data.append(0x04 | ((sz - 1) << 5))
                        data.extend(b[:sz])
                    else:
                        raise ValueError(f"Unsupported static init type {fld_t}")
                sval_count += 1
            else:
                sval_offsets[c["cls"]] = 0
        if sval_count:
            map_items.append((0x2005, sval_count, sval_start))

        # Section: class_data_items
        cdata_start = data_off + len(data)
        cdata_offsets = {}
        for c in cls_list:
            cdata_offsets[c["cls"]] = data_off + len(data)
            sfs = sorted(c["static_fields"], key=lambda x: field_idx[(c["cls"], x[0], x[1])])
            ifs = sorted(c["instance_fields"], key=lambda x: field_idx[(c["cls"], x[0], x[1])])
            dms = sorted(c["direct_methods"], key=lambda m: method_idx[(c["cls"], m["name"], m["ret"], tuple(m["params"]))])
            vms = sorted(c["virtual_methods"], key=lambda m: method_idx[(c["cls"], m["name"], m["ret"], tuple(m["params"]))])
            data.extend(uleb128(len(sfs)))
            data.extend(uleb128(len(ifs)))
            data.extend(uleb128(len(dms)))
            data.extend(uleb128(len(vms)))
            prev = 0
            for fld_t, name, acc, _ in sfs:
                fid = field_idx[(c["cls"], fld_t, name)]
                data.extend(uleb128(fid - prev))
                data.extend(uleb128(acc))
                prev = fid
            prev = 0
            for fld_t, name, acc in ifs:
                fid = field_idx[(c["cls"], fld_t, name)]
                data.extend(uleb128(fid - prev))
                data.extend(uleb128(acc))
                prev = fid
            for mlist in (dms, vms):
                prev = 0
                for m in mlist:
                    key = (c["cls"], m["name"], m["ret"], tuple(m["params"]))
                    mid = method_idx[key]
                    data.extend(uleb128(mid - prev))
                    data.extend(uleb128(m["access"]))
                    data.extend(uleb128(code_offsets.get(key, 0)))
                    prev = mid
        map_items.append((0x2000, len(cls_list), cdata_start))

        # Section: map_list
        align4(data)
        map_off = data_off + len(data)
        map_items.append((0x1000, 1, map_off))
        data.extend(struct.pack("<I", len(map_items)))
        for mtype, mcnt, moff in map_items:
            data.extend(struct.pack("<HHII", mtype, 0, mcnt, moff))

        # Assemble full DEX file
        out = bytearray(data_off)
        # Write string_ids
        for i, off in enumerate(str_data_offsets):
            struct.pack_into("<I", out, str_ids_off + i * 4, off)
        # Write type_ids
        for i, t in enumerate(type_list):
            struct.pack_into("<I", out, type_ids_off + i * 4, str_idx[t])
        # Write proto_ids
        for i, p in enumerate(proto_list):
            sh = shorty_of(p[0], list(p[1]))
            struct.pack_into("<III", out, proto_ids_off + i * 12, str_idx[sh], type_idx[p[0]], type_list_offsets[p])
        # Write field_ids
        for i, f in enumerate(field_list):
            struct.pack_into("<HHI", out, field_ids_off + i * 8, type_idx[f[0]], type_idx[f[1]], str_idx[f[2]])
        # Write method_ids
        for i, m in enumerate(method_list):
            struct.pack_into("<HHI", out, method_ids_off + i * 8, type_idx[m[0]], proto_idx[(m[2], m[3])], str_idx[m[1]])
        # Write class_defs
        for i, c in enumerate(cls_list):
            struct.pack_into(
                "<8I", out, class_defs_off + i * 32,
                type_idx[c["cls"]],
                c["access"],
                type_idx[c["super"]],
                iface_offsets.get(c["cls"], 0),
                0xFFFFFFFF,  # source_file_idx (NO_INDEX)
                0,  # annotations_off
                cdata_offsets[c["cls"]],
                sval_offsets[c["cls"]],
            )

        out.extend(data)
        file_size = len(out)
        data_size = file_size - data_off

        # Write DEX 035 header
        out[0:8] = b"dex\n035\x00"
        struct.pack_into(
            "<20I", out, 32,
            file_size, header_size, 0x12345678,
            0, 0, map_off,
            len(str_list), str_ids_off,
            len(type_list), type_ids_off,
            len(proto_list), proto_ids_off,
            len(field_list), field_ids_off if field_list else 0,
            len(method_list), method_ids_off if method_list else 0,
            len(cls_list), class_defs_off,
            data_size, data_off,
        )
        out[12:32] = hashlib.sha1(out[32:]).digest()
        adler = zlib.adler32(out[12:]) & 0xFFFFFFFF
        struct.pack_into("<I", out, 8, adler)
        return bytes(out)


# ---- Bytecode helpers ----

def m_super_init(super_t: str, params: list[str], super_params: list[str] = None):
    sp = list(params) if super_params is None else list(super_params)
    in_regs = 1 + len(params)  # this + params (all 1-word types here)
    out_regs = 1 + len(sp)

    def pre(b: DexBuilder):
        b.add_method_ref(super_t, "<init>", "V", tuple(sp))

    def body(ctx):
        mid = ctx["method"][(super_t, "<init>", "V", tuple(sp))]
        # invoke-direct/range {v0 .. v(out_regs-1)}, super.<init>
        insns = [0x0076 | (out_regs << 8), mid, 0, 0x000E]
        return (in_regs, in_regs, out_regs, insns)

    return {"name": "<init>", "ret": "V", "params": params, "access": ACC_PUBLIC | ACC_CONSTRUCTOR, "pre_reg": pre, "body_fn": body}


def m_ret_void(name: str, params: list[str], static=False):
    in_regs = (0 if static else 1) + len(params)
    return {
        "name": name, "ret": "V", "params": params,
        "access": ACC_PUBLIC | (ACC_STATIC if static else 0),
        "pre_reg": lambda b: None,
        "body_fn": lambda ctx: (max(1, in_regs), in_regs, 0, [0x000E]),
    }


def m_ret_const_int(name: str, ret_t: str, params: list[str], val: int, static=False):
    in_regs = (0 if static else 1) + len(params)
    assert -8 <= val <= 7
    nib = val & 0xF
    return {
        "name": name, "ret": ret_t, "params": params,
        "access": ACC_PUBLIC | (ACC_STATIC if static else 0),
        "pre_reg": lambda b: None,
        "body_fn": lambda ctx: (max(1, in_regs), in_regs, 0, [(nib << 12) | 0x0012, 0x000F]),
    }


def m_ret_const_float(name: str, params: list[str], fval: float, static=False):
    in_regs = (0 if static else 1) + len(params)
    bits = struct.unpack("<I", struct.pack("<f", fval))[0]
    lo = bits & 0xFFFF
    hi = (bits >> 16) & 0xFFFF
    return {
        "name": name, "ret": "F", "params": params,
        "access": ACC_PUBLIC | (ACC_STATIC if static else 0),
        "pre_reg": lambda b: None,
        "body_fn": lambda ctx: (max(1, in_regs), in_regs, 0, [0x0014, lo, hi, 0x000F]),  # const v0, #bits; return v0
    }


def m_ret_const_str(name: str, params: list[str], sval: str, static=False):
    in_regs = (0 if static else 1) + len(params)

    def pre(b: DexBuilder):
        b.add_string(sval)

    def body(ctx):
        si = ctx["str"][sval]
        return (max(1, in_regs), in_regs, 0, [0x001A, si, 0x0011])  # const-string v0, si; return-object v0

    return {
        "name": name, "ret": "Ljava/lang/String;", "params": params,
        "access": ACC_PUBLIC | (ACC_STATIC if static else 0),
        "pre_reg": pre, "body_fn": body,
    }


def m_ret_null(name: str, ret_t: str, params: list[str], static=False):
    in_regs = (0 if static else 1) + len(params)
    return {
        "name": name, "ret": ret_t, "params": params,
        "access": ACC_PUBLIC | (ACC_STATIC if static else 0),
        "pre_reg": lambda b: None,
        "body_fn": lambda ctx: (max(1, in_regs), in_regs, 0, [0x0012, 0x0011]),  # const/4 v0, 0; return-object v0
    }


def m_ret_arg1(name: str, ret_t: str, params: list[str]):
    # Instance method returning 2nd argument (index 2: v0=this, v1=arg0, v2=arg1)
    in_regs = 1 + len(params)
    return {
        "name": name, "ret": ret_t, "params": params,
        "access": ACC_PUBLIC,
        "pre_reg": lambda b: None,
        "body_fn": lambda ctx: (in_regs, in_regs, 0, [0x020F]),  # return v2
    }


def m_singleton_get_instance(cls_t: str):
    def pre(b: DexBuilder):
        b.add_field_ref(cls_t, cls_t, "sInstance")
        b.add_method_ref(cls_t, "<init>", "V", ())

    def body(ctx):
        fid = ctx["field"][(cls_t, cls_t, "sInstance")]
        tid = ctx["type"][cls_t]
        mid = ctx["method"][(cls_t, "<init>", "V", ())]
        # 0: sget-object v0, sInstance          (2u)
        # 2: if-nez v0, +7 (target = 9)         (2u)
        # 4: new-instance v0, cls_t             (2u)
        # 6: invoke-direct {v0}, cls_t.<init>() (3u)
        # 9: sput-object v0, sInstance          (2u) -> wait if target=9, skip sput! Target should be 11!
        # Let's place target at 11:
        # 0: sget-object v0, fid       (0x0062, fid)
        # 2: if-nez v0, +9 -> 11       (0x0039, 9)
        # 4: new-instance v0, tid      (0x0022, tid)
        # 6: invoke-direct {v0}, mid   (0x1070, mid, 0x0000)
        # 9: sput-object v0, fid       (0x0069, fid)
        # 11: return-object v0         (0x0011)
        insns = [0x0062, fid, 0x0039, 9, 0x0022, tid, 0x1070, mid, 0x0000, 0x0069, fid, 0x0011]
        return (1, 0, 1, insns)

    return {
        "name": "getInstance", "ret": cls_t, "params": [],
        "access": ACC_PUBLIC | ACC_STATIC,
        "pre_reg": pre, "body_fn": body,
    }


def build_compat_dex(out_path: str):
    b = DexBuilder()
    OBJ = "Ljava/lang/Object;"
    STR = "Ljava/lang/String;"
    CTX = "Landroid/content/Context;"
    ATTR = "Landroid/util/AttributeSet;"

    # 1. android.os.FtBuild (PD2338 OriginOS 4 / iQOO Neo9)
    b.add_class(
        "Landroid/os/FtBuild;", OBJ, ACC_PUBLIC,
        direct_methods=[
            m_super_init(OBJ, []),
            m_ret_const_str("getFirstOsVersion", [], "14.0", static=True),
            m_ret_const_float("getRomVersion", [], 14.1, static=True),
            m_ret_const_str("getOsName", [], "Funtouch", static=True),
            m_ret_const_str("getOsVersion", [], "14.1", static=True),
            m_ret_const_str("getProductName", [], "PD2338", static=True),
            m_ret_const_str("getProductReleaseName", [], "PD2338", static=True),
            m_ret_const_str("getProductSeries", [], "IQOO", static=True),
            m_ret_const_int("isQCOMPlatform", "Z", [], 1, static=True),
            m_ret_const_int("isMTKPlatform", "Z", [], 0, static=True),
            m_ret_const_int("isOverSeas", "Z", [], 0, static=True),
        ],
    )

    # 2. android.media.AudioFeatures + inner classes
    AFC = "Landroid/media/AudioFeatures$AudioFeatureCallback;"
    b.add_class(
        AFC, OBJ, ACC_PUBLIC,
        direct_methods=[m_super_init(OBJ, [CTX, STR, OBJ], [])],
        virtual_methods=[m_ret_null("onCallback", STR, [STR, OBJ])],
    )
    b.add_class(
        "Landroid/media/AudioFeatures$TagParameters;", OBJ, ACC_PUBLIC,
        direct_methods=[m_super_init(OBJ, [STR], [])],
        virtual_methods=[
            m_ret_arg1("getInt", "I", [STR, "I"]),
            m_ret_arg1("getBoolean", "Z", [STR, "Z"]),
            m_ret_null("get", STR, [STR]),
        ],
    )
    b.add_class(
        "Landroid/media/AudioFeatures;", OBJ, ACC_PUBLIC,
        direct_methods=[m_super_init(OBJ, [CTX, STR, OBJ], [])],
        virtual_methods=[
            m_ret_const_str("getAudioFeature", [STR, OBJ], ""),
            m_ret_const_str("setAudioFeature", [STR, OBJ], ""),
            m_ret_const_str("registerAudioFeatureCallback", [AFC, STR, OBJ], ""),
            m_ret_const_str("unregisterAudioFeatureCallback", [AFC, STR, OBJ], ""),
        ],
    )

    # 3. android.preference.VivoCheckBoxPreference extends android.preference.CheckBoxPreference
    CBP = "Landroid/preference/CheckBoxPreference;"
    b.add_class(
        "Landroid/preference/VivoCheckBoxPreference;", CBP, ACC_PUBLIC,
        direct_methods=[
            m_super_init(CBP, [CTX]),
            m_super_init(CBP, [CTX, ATTR]),
            m_super_init(CBP, [CTX, ATTR, "I"]),
        ],
    )

    # 4. android.widget.BbkMoveBoolButton (+ $OnCheckedChangeListener)
    # Matches OriginOS 4 framework: extends android.widget.ImageView implements android.widget.Checkable
    BBK_BTN = "Landroid/widget/BbkMoveBoolButton;"
    BBK_LST = "Landroid/widget/BbkMoveBoolButton$OnCheckedChangeListener;"
    IMGV = "Landroid/widget/ImageView;"
    CHK = "Landroid/widget/Checkable;"
    b.add_class(
        BBK_LST, OBJ, ACC_PUBLIC | ACC_INTERFACE | ACC_ABSTRACT,
        virtual_methods=[{
            "name": "onCheckedChanged", "ret": "V", "params": [BBK_BTN, "Z"],
            "access": ACC_PUBLIC | ACC_ABSTRACT, "pre_reg": lambda _: None, "body_fn": None,
        }],
    )
    b.add_class(
        BBK_BTN, IMGV, ACC_PUBLIC,
        interfaces=[CHK],
        instance_fields=[("Z", "mChecked", 0x0002)],  # ACC_PRIVATE
        direct_methods=[
            m_super_init(IMGV, [CTX]),
            m_super_init(IMGV, [CTX, ATTR]),
            m_super_init(IMGV, [CTX, ATTR, "I"]),
            m_super_init(IMGV, [CTX, ATTR, "I", "I"]),
        ],
        virtual_methods=[
            {
                "name": "isChecked", "ret": "Z", "params": [], "access": ACC_PUBLIC,
                "pre_reg": lambda builder: builder.add_field_ref(BBK_BTN, "Z", "mChecked"),
                "body_fn": lambda ctx: (1, 1, 0, [0x0055, ctx["field"][(BBK_BTN, "Z", "mChecked")], 0x000F]),
            },
            {
                "name": "setChecked", "ret": "V", "params": ["Z"], "access": ACC_PUBLIC,
                "pre_reg": lambda builder: builder.add_field_ref(BBK_BTN, "Z", "mChecked"),
                "body_fn": lambda ctx: (2, 2, 0, [0x015C, ctx["field"][(BBK_BTN, "Z", "mChecked")], 0x000E]),
            },
            {
                "name": "toggle", "ret": "V", "params": [], "access": ACC_PUBLIC,
                "pre_reg": lambda builder: builder.add_field_ref(BBK_BTN, "Z", "mChecked"),
                # regs=2, ins=1 -> incoming `this` is in v1 (highest register), v0 is scratch!
                "body_fn": lambda ctx: (2, 1, 0, [
                    0x1055, ctx["field"][(BBK_BTN, "Z", "mChecked")],  # iget-boolean v0, v1, mChecked
                    0x00DF, 0x0100,                                     # xor-int/lit8 v0, v0, #1
                    0x105C, ctx["field"][(BBK_BTN, "Z", "mChecked")],  # iput-boolean v0, v1, mChecked
                    0x000E,                                             # return-void
                ]),
            },
            m_ret_void("setOnBBKCheckedChangeListener", [BBK_LST]),
        ],
    )

    # 5. com.vivo.app.VivoPreferenceActivity extends android.preference.PreferenceActivity
    PA = "Landroid/preference/PreferenceActivity;"
    b.add_class(
        "Lcom/vivo/app/VivoPreferenceActivity;", PA, ACC_PUBLIC,
        direct_methods=[m_super_init(PA, [])],
    )

    # 6. com.vivo.common.BbkTitleView extends android.widget.RelativeLayout
    RL = "Landroid/widget/RelativeLayout;"
    b.add_class(
        "Lcom/vivo/common/BbkTitleView;", RL, ACC_PUBLIC,
        static_fields=[
            ("I", "TITLE_BTN_NORMAL", ACC_PUBLIC | ACC_STATIC | ACC_FINAL, 1),
            ("I", "TITLE_BTN_BACK", ACC_PUBLIC | ACC_STATIC | ACC_FINAL, 2),
            ("I", "TITLE_BTN_CREATE", ACC_PUBLIC | ACC_STATIC | ACC_FINAL, 3),
            ("I", "TITLE_BTN_NEW", ACC_PUBLIC | ACC_STATIC | ACC_FINAL, 4),
        ],
        direct_methods=[
            m_super_init(RL, [CTX]),
            m_super_init(RL, [CTX, ATTR]),
        ],
    )

    # 7. vivo.app.themeicon.{SystemColorListener, SystemColorWheelListener, SystemFilletListener}
    SCL = "Lvivo/app/themeicon/SystemColorListener;"
    SCWL = "Lvivo/app/themeicon/SystemColorWheelListener;"
    SFL = "Lvivo/app/themeicon/SystemFilletListener;"
    b.add_class(SCL, OBJ, ACC_PUBLIC, direct_methods=[m_super_init(OBJ, [])],
                virtual_methods=[m_ret_void("onSystemColorChanged", ["I", "I", "I"])])
    b.add_class(SCWL, OBJ, ACC_PUBLIC, direct_methods=[m_super_init(OBJ, [])],
                virtual_methods=[m_ret_void("onSystemColorWheelChanged", ["I", "[I"])])
    b.add_class(SFL, OBJ, ACC_PUBLIC, direct_methods=[m_super_init(OBJ, [])],
                virtual_methods=[m_ret_void("onSystemFilletChanged", ["I", "I"])])

    # 8. com.vivo.framework.themeicon.ThemeIconManager
    TIM = "Lcom/vivo/framework/themeicon/ThemeIconManager;"
    b.add_class(
        TIM, OBJ, ACC_PUBLIC,
        static_fields=[(TIM, "sInstance", ACC_STATIC, None)],
        direct_methods=[m_super_init(OBJ, []), m_singleton_get_instance(TIM)],
        virtual_methods=[
            m_ret_const_int("getSystemColorMode", "I", [], 0),
            m_ret_null("getSystemColorWheelIntArray", "[I", []),
            m_ret_const_int("getSystemFillet", "I", [], 0),
            m_ret_const_int("getSystemFilletLevel", "I", [], 0),
            m_ret_const_int("getSystemPrimaryColor", "I", [], 0),
            m_ret_const_int("getSystemSecondaryColor", "I", [], 0),
            m_ret_const_int("registerSystemColorChangeListener", "Z", [SCL], 0),
            m_ret_const_int("registerSystemColorWheelChangeListener", "Z", [SCWL], 0),
            m_ret_const_int("registerSystemFilletChangeListener", "Z", [SFL], 0),
            m_ret_const_int("unregisterSystemColorChangeListener", "Z", [SCL], 0),
            m_ret_const_int("unregisterSystemColorWheelChangeListener", "Z", [SCWL], 0),
            m_ret_const_int("unregisterSystemFilletChangeListener", "Z", [SFL], 0),
        ],
    )

    # 9. com.vivo.framework.touchscreen.TouchScreenManager
    TSM = "Lcom/vivo/framework/touchscreen/TouchScreenManager;"
    b.add_class(
        TSM, OBJ, ACC_PUBLIC,
        static_fields=[(TSM, "sInstance", ACC_STATIC, None)],
        direct_methods=[m_super_init(OBJ, []), m_singleton_get_instance(TSM)],
    )

    # 10. com.vivo.common.utils.VLog (required by genuine android.hardware.vivocamera.* in classes16.dex!)
    # Forwards directly to android.util.Log so VivoCameraManager / VCF2 logs appear cleanly in logcat.
    LOG = "Landroid/util/Log;"
    THR = "Ljava/lang/Throwable;"

    def m_log_fwd(name: str, ret_t: str, params: list[str]):
        n = len(params)

        def pre(builder: DexBuilder):
            builder.add_method_ref(LOG, name, ret_t, tuple(params))

        def body(ctx):
            mid = ctx["method"][(LOG, name, ret_t, tuple(params))]
            # regs = max(1, n), ins = n, outs = n; params in v0..v(n-1)
            # invoke-static/range {v0 .. v(n-1)}, mid  ->  0x0077 | (n << 8), mid, 0
            # move-result v0 (0x000A), return v0 (0x000F)
            insns = [0x0077 | (n << 8), mid, 0, 0x000A, 0x000F]
            return (max(1, n), n, n, insns)

        return {
            "name": name, "ret": ret_t, "params": params,
            "access": ACC_PUBLIC | ACC_STATIC,
            "pre_reg": pre, "body_fn": body,
        }

    b.add_class(
        "Lcom/vivo/common/utils/VLog;", OBJ, ACC_PUBLIC,
        direct_methods=[
            m_super_init(OBJ, []),
            m_log_fwd("v", "I", [STR, STR]),
            m_log_fwd("v", "I", [STR, STR, THR]),
            m_log_fwd("d", "I", [STR, STR]),
            m_log_fwd("d", "I", [STR, STR, THR]),
            m_log_fwd("i", "I", [STR, STR]),
            m_log_fwd("i", "I", [STR, STR, THR]),
            m_log_fwd("w", "I", [STR, STR]),
            m_log_fwd("w", "I", [STR, THR]),
            m_log_fwd("w", "I", [STR, STR, THR]),
            m_log_fwd("e", "I", [STR, STR]),
            m_log_fwd("e", "I", [STR, STR, THR]),
            m_log_fwd("wtf", "I", [STR, STR]),
            m_log_fwd("wtf", "I", [STR, STR, THR]),
            m_log_fwd("isLoggable", "Z", [STR, "I"]),
        ],
    )

    # 11. Real hardware VivoPerfManager -> vendor.vivo.hardware.vperf@1.0::IVPerf/default via android.os.HwBinder!
    # Unlocks BOTH VivoBoostFrameworkUtil (launch/switch/shutter CPU+GPU boosts) AND VivoCpuLimitUtil
    # (sustained thermal/power CPU frequency ceilings) against the live vendor hal_vperf_default daemon!
    b.add_class("Lvivo/app/vperf/AbsVivoPerfManager;", OBJ, ACC_PUBLIC | ACC_ABSTRACT,
                direct_methods=[m_super_init(OBJ, [])])
    VPM = "Lcom/vivo/framework/vperf/VivoPerfManager;"
    HWB = "Landroid/os/HwBinder;"
    IHWB = "Landroid/os/IHwBinder;"
    HWP = "Landroid/os/HwParcel;"
    AL = "Ljava/util/ArrayList;"
    INT = "Ljava/lang/Integer;"
    PROC = "Landroid/os/Process;"
    VPERF_IFACE = "vendor.vivo.hardware.vperf@1.0::IVPerf"
    VPERF_INST = "default"

    def pre_get_vperf(builder: DexBuilder):
        builder.add_string(VPERF_IFACE)
        builder.add_string(VPERF_INST)
        builder.add_field_ref(VPM, IHWB, "sVPerf")
        builder.add_method_ref(HWB, "getService", IHWB, (STR, STR, "Z"))

    def body_get_vperf(ctx):
        f_svp = ctx["field"][(VPM, IHWB, "sVPerf")]
        s_if = ctx["str"][VPERF_IFACE]
        s_def = ctx["str"][VPERF_INST]
        m_get = ctx["method"][(HWB, "getService", IHWB, (STR, STR, "Z"))]
        code = []
        code += [0x0062, f_svp]                     # 00: sget-object v0, sVPerf
        code += [0x0038, 3]                         # 02: if-eqz v0, +3 (-> 05)
        code += [0x0011]                            # 04: return-object v0
        try_s = len(code)                           # 05
        code += [0x001A, s_if]                      # 05: const-string v0, VPERF_IFACE
        code += [0x011A, s_def]                     # 07: const-string v1, "default"
        code += [0x0212]                            # 09: const/4 v2, 0 (retry=false)
        code += enc_35c(0x71, m_get, [0, 1, 2])     # 0a: invoke-static {v0,v1,v2}, HwBinder.getService
        code += [0x000C]                            # 0d: move-result-object v0
        code += [0x0069, f_svp]                     # 0e: sput-object v0, sVPerf
        try_e = len(code)                           # 10
        code += [0x0011]                            # 10: return-object v0
        catch_u = len(code)                         # 11
        code += [0x000D, 0x0012, 0x0011]            # 11: move-exception v0; const/4 v0, 0; return-object v0
        return (3, 0, 3, code, [(try_s, try_e - try_s, catch_u)])

    def pre_perf_acq(builder: DexBuilder):
        builder.add_string(VPERF_IFACE)
        builder.add_type(HWP)
        builder.add_type(AL)
        builder.add_field_ref(VPM, IHWB, "sVPerf")
        builder.add_field_ref(VPM, "I", "mHandle")
        builder.add_method_ref(VPM, "getVPerf", IHWB, ())
        builder.add_method_ref(HWP, "<init>", "V", ())
        builder.add_method_ref(HWP, "writeInterfaceToken", "V", (STR,))
        builder.add_method_ref(HWP, "writeInt32", "V", ("I",))
        builder.add_method_ref(HWP, "writeInt32Vector", "V", (AL,))
        builder.add_method_ref(HWP, "verifySuccess", "V", ())
        builder.add_method_ref(HWP, "releaseTemporaryStorage", "V", ())
        builder.add_method_ref(HWP, "readInt32", "I", ())
        builder.add_method_ref(HWP, "release", "V", ())
        builder.add_method_ref(IHWB, "transact", "V", ("I", HWP, HWP, "I"))
        builder.add_method_ref(AL, "<init>", "V", ("I",))
        builder.add_method_ref(AL, "add", "Z", (OBJ,))
        builder.add_method_ref(INT, "valueOf", INT, ("I",))
        builder.add_method_ref(PROC, "myTid", "I", ())
        builder.add_method_ref(PROC, "myPid", "I", ())

    def body_perf_acq(ctx):
        # regs=10, ins=3, outs=5 -> v7=this, v8=duration, v9=list; scratch v0..v6
        f_svp = ctx["field"][(VPM, IHWB, "sVPerf")]
        f_mh = ctx["field"][(VPM, "I", "mHandle")]
        s_if = ctx["str"][VPERF_IFACE]
        t_hwp = ctx["type"][HWP]
        t_al = ctx["type"][AL]
        m_get = ctx["method"][(VPM, "getVPerf", IHWB, ())]
        m_hwp_init = ctx["method"][(HWP, "<init>", "V", ())]
        m_w_tok = ctx["method"][(HWP, "writeInterfaceToken", "V", (STR,))]
        m_w_i32 = ctx["method"][(HWP, "writeInt32", "V", ("I",))]
        m_w_vec = ctx["method"][(HWP, "writeInt32Vector", "V", (AL,))]
        m_vsuc = ctx["method"][(HWP, "verifySuccess", "V", ())]
        m_rtmp = ctx["method"][(HWP, "releaseTemporaryStorage", "V", ())]
        m_r_i32 = ctx["method"][(HWP, "readInt32", "I", ())]
        m_rel = ctx["method"][(HWP, "release", "V", ())]
        m_tx = ctx["method"][(IHWB, "transact", "V", ("I", HWP, HWP, "I"))]
        m_al_init = ctx["method"][(AL, "<init>", "V", ("I",))]
        m_al_add = ctx["method"][(AL, "add", "Z", (OBJ,))]
        m_valof = ctx["method"][(INT, "valueOf", INT, ("I",))]
        m_tid = ctx["method"][(PROC, "myTid", "I", ())]
        m_pid = ctx["method"][(PROC, "myPid", "I", ())]
        code = []
        code += [0x0939, 4]                         # if-nez v9, +4 (skip ret -1)
        code += [0xF012, 0x000F]                    # const/4 v0, -1; return v0
        code += enc_35c(0x71, m_get, [])            # invoke-static {}, getVPerf
        code += [0x000C]                            # move-result-object v0
        code += [0x0039, 4]                         # if-nez v0, +4 (skip ret -1)
        code += [0xF012, 0x000F]                    # const/4 v0, -1; return v0
        try_s = len(code)
        code += [0x0122, t_hwp]                     # new-instance v1, HwParcel
        code += enc_35c(0x70, m_hwp_init, [1])      # invoke-direct {v1}, HwParcel.<init>()
        code += [0x021A, s_if]                      # const-string v2, VPERF_IFACE
        code += enc_35c(0x6E, m_w_tok, [1, 2])      # v1.writeInterfaceToken(v2)
        code += [0x0212]                            # const/4 v2, 0 (pl_handle=0)
        code += enc_35c(0x6E, m_w_i32, [1, 2])      # v1.writeInt32(0)
        code += enc_35c(0x6E, m_w_i32, [1, 8])      # v1.writeInt32(duration)
        code += [0x9221]                            # array-length v2, v9
        code += [0x0322, t_al]                      # new-instance v3, ArrayList
        code += enc_35c(0x70, m_al_init, [3, 2])    # new ArrayList(n)
        code += [0x0412]                            # const/4 v4, 0 (i=0)
        loop_u = len(code)
        j_idx = len(code)
        code += [0x2435, 0]                         # if-ge v4, v2, loop_end (patched below)
        code += [0x0544, 0x0409]                    # aget v5, v9, v4
        code += enc_35c(0x71, m_valof, [5])         # Integer.valueOf(v5)
        code += [0x050C]                            # move-result-object v5
        code += enc_35c(0x6E, m_al_add, [3, 5])     # v3.add(v5)
        code += [0x04D8, 0x0104]                    # add-int/lit8 v4, v4, 1
        back_delta = (loop_u - len(code)) & 0xFFFF
        code += [0x0029, back_delta]                # goto/16 loop_u
        loop_end_u = len(code)
        code[j_idx + 1] = (loop_end_u - j_idx) & 0xFFFF
        code += enc_35c(0x6E, m_w_vec, [1, 3])      # v1.writeInt32Vector(al)
        # reserved = [Process.myTid(), Process.myPid()]
        code += [0x2212]                            # const/4 v2, 2
        code += [0x0322, t_al]                      # new-instance v3, ArrayList
        code += enc_35c(0x70, m_al_init, [3, 2])    # new ArrayList(2)
        code += enc_35c(0x71, m_tid, [])            # Process.myTid()
        code += [0x040A]                            # move-result v4
        code += enc_35c(0x71, m_valof, [4])         # Integer.valueOf(v4)
        code += [0x040C]                            # move-result-object v4
        code += enc_35c(0x6E, m_al_add, [3, 4])     # v3.add(v4)
        code += enc_35c(0x71, m_pid, [])            # Process.myPid()
        code += [0x040A]                            # move-result v4
        code += enc_35c(0x71, m_valof, [4])         # Integer.valueOf(v4)
        code += [0x040C]                            # move-result-object v4
        code += enc_35c(0x6E, m_al_add, [3, 4])     # v3.add(v4)
        code += enc_35c(0x6E, m_w_vec, [1, 3])      # v1.writeInt32Vector(reserved)
        # HwParcel reply & transact(2, req, rep, 0)
        code += [0x0222, t_hwp]                     # new-instance v2, HwParcel
        code += enc_35c(0x70, m_hwp_init, [2])      # invoke-direct {v2}, HwParcel.<init>()
        code += [0x2312]                            # const/4 v3, 2 (tx=perfLockAcquire)
        code += [0x0412]                            # const/4 v4, 0 (flags=0)
        code += enc_35c(0x72, m_tx, [0, 3, 1, 2, 4])# v0.transact(2, v1, v2, 0)
        code += enc_35c(0x6E, m_vsuc, [2])          # v2.verifySuccess()
        code += enc_35c(0x6E, m_rtmp, [1])          # v1.releaseTemporaryStorage()
        code += enc_35c(0x6E, m_r_i32, [2])         # v2.readInt32()
        code += [0x040A]                            # move-result v4
        code += enc_35c(0x6E, m_rel, [1])           # v1.release()
        code += enc_35c(0x6E, m_rel, [2])           # v2.release()
        code += [0x7459, f_mh]                      # iput v4, v7, mHandle
        try_e = len(code)
        code += [0x040F]                            # return v4
        catch_u = len(code)
        code += [0x000D, 0x0012, 0x0069, f_svp, 0xF012, 0x000F]  # move-exc; sVPerf=null; return -1
        return (10, 3, 5, code, [(try_s, try_e - try_s, catch_u)])

    def pre_perf_rel_h(builder: DexBuilder):
        pre_perf_acq(builder)

    def body_perf_rel_h(ctx):
        # regs=8, ins=2, outs=5 -> v6=this, v7=handle; scratch v0..v5
        f_svp = ctx["field"][(VPM, IHWB, "sVPerf")]
        s_if = ctx["str"][VPERF_IFACE]
        t_hwp = ctx["type"][HWP]
        t_al = ctx["type"][AL]
        m_get = ctx["method"][(VPM, "getVPerf", IHWB, ())]
        m_hwp_init = ctx["method"][(HWP, "<init>", "V", ())]
        m_w_tok = ctx["method"][(HWP, "writeInterfaceToken", "V", (STR,))]
        m_w_i32 = ctx["method"][(HWP, "writeInt32", "V", ("I",))]
        m_w_vec = ctx["method"][(HWP, "writeInt32Vector", "V", (AL,))]
        m_vsuc = ctx["method"][(HWP, "verifySuccess", "V", ())]
        m_rtmp = ctx["method"][(HWP, "releaseTemporaryStorage", "V", ())]
        m_rel = ctx["method"][(HWP, "release", "V", ())]
        m_tx = ctx["method"][(IHWB, "transact", "V", ("I", HWP, HWP, "I"))]
        m_al_init = ctx["method"][(AL, "<init>", "V", ("I",))]
        m_al_add = ctx["method"][(AL, "add", "Z", (OBJ,))]
        m_valof = ctx["method"][(INT, "valueOf", INT, ("I",))]
        m_tid = ctx["method"][(PROC, "myTid", "I", ())]
        m_pid = ctx["method"][(PROC, "myPid", "I", ())]
        code = []
        code += [0x073C, 4]                         # if-gtz v7, +4 (skip ret 0)
        code += [0x0012, 0x000F]                    # const/4 v0, 0; return v0
        code += enc_35c(0x71, m_get, [])            # getVPerf()
        code += [0x000C]                            # move-result-object v0
        code += [0x0039, 4]                         # if-nez v0, +4 (skip ret -1)
        code += [0xF012, 0x000F]                    # const/4 v0, -1; return v0
        try_s = len(code)
        code += [0x0122, t_hwp]                     # new-instance v1, HwParcel
        code += enc_35c(0x70, m_hwp_init, [1])
        code += [0x021A, s_if]
        code += enc_35c(0x6E, m_w_tok, [1, 2])
        code += enc_35c(0x6E, m_w_i32, [1, 7])      # v1.writeInt32(handle)
        code += [0x2212]                            # const/4 v2, 2
        code += [0x0322, t_al]
        code += enc_35c(0x70, m_al_init, [3, 2])
        code += enc_35c(0x71, m_tid, [])
        code += [0x040A]
        code += enc_35c(0x71, m_valof, [4])
        code += [0x040C]
        code += enc_35c(0x6E, m_al_add, [3, 4])
        code += enc_35c(0x71, m_pid, [])
        code += [0x040A]
        code += enc_35c(0x71, m_valof, [4])
        code += [0x040C]
        code += enc_35c(0x6E, m_al_add, [3, 4])
        code += enc_35c(0x6E, m_w_vec, [1, 3])
        code += [0x0222, t_hwp]
        code += enc_35c(0x70, m_hwp_init, [2])
        code += [0x3312]                            # const/4 v3, 3 (tx=perfLockRelease)
        code += [0x0412]                            # const/4 v4, 0 (flags=0)
        code += enc_35c(0x72, m_tx, [0, 3, 1, 2, 4])
        code += enc_35c(0x6E, m_vsuc, [2])
        code += enc_35c(0x6E, m_rtmp, [1])
        code += enc_35c(0x6E, m_rel, [1])
        code += enc_35c(0x6E, m_rel, [2])
        try_e = len(code)
        code += [0x0012, 0x000F]                    # const/4 v0, 0; return v0
        catch_u = len(code)
        code += [0x000D, 0x0012, 0x0069, f_svp, 0xF012, 0x000F]
        return (8, 2, 5, code, [(try_s, try_e - try_s, catch_u)])

    def pre_perf_rel(builder: DexBuilder):
        builder.add_field_ref(VPM, "I", "mHandle")
        builder.add_method_ref(VPM, "perfLockReleaseHandler", "I", ("I",))

    def body_perf_rel(ctx):
        # regs=3, ins=1, outs=2 -> v2=this; scratch v0, v1
        f_mh = ctx["field"][(VPM, "I", "mHandle")]
        m_rh = ctx["method"][(VPM, "perfLockReleaseHandler", "I", ("I",))]
        code = [
            0x2052, f_mh,                           # iget v0, v2, mHandle
            0x0112,                                 # const/4 v1, 0
            0x2159, f_mh,                           # iput v1, v2, mHandle
            *enc_35c(0x6E, m_rh, [2, 0]),           # invoke-virtual {v2, v0}, perfLockReleaseHandler(I)I
            0x000A,                                 # move-result v0
            0x000F,                                 # return v0
        ]
        return (3, 1, 2, code)

    b.add_class(
        VPM, "Lvivo/app/vperf/AbsVivoPerfManager;", ACC_PUBLIC,
        static_fields=[(IHWB, "sVPerf", 0x000A, None)],
        instance_fields=[("I", "mHandle", 0x0002)],
        direct_methods=[
            m_super_init("Lvivo/app/vperf/AbsVivoPerfManager;", []),
            m_super_init("Lvivo/app/vperf/AbsVivoPerfManager;", [CTX], []),
            {"name": "getVPerf", "ret": IHWB, "params": [], "access": 0x000A,
             "pre_reg": pre_get_vperf, "body_fn": body_get_vperf},
        ],
        virtual_methods=[
            {"name": "perfLockAcquire", "ret": "I", "params": ["I", "[I"], "access": ACC_PUBLIC,
             "pre_reg": pre_perf_acq, "body_fn": body_perf_acq},
            {"name": "perfLockRelease", "ret": "I", "params": [], "access": ACC_PUBLIC,
             "pre_reg": pre_perf_rel, "body_fn": body_perf_rel},
            {"name": "perfLockReleaseHandler", "ret": "I", "params": ["I"], "access": ACC_PUBLIC,
             "pre_reg": pre_perf_rel_h, "body_fn": body_perf_rel_h},
        ],
    )
    b.add_class("Lvivo/app/popupcamera/AbsPopupCameraManager;", OBJ, ACC_PUBLIC | ACC_ABSTRACT,
                direct_methods=[m_super_init(OBJ, [])])
    PCM = "Lcom/vivo/framework/popupcamera/PopupCameraManager;"
    b.add_class(
        PCM, "Lvivo/app/popupcamera/AbsPopupCameraManager;", ACC_PUBLIC,
        static_fields=[(PCM, "sInstance", ACC_STATIC, None)],
        direct_methods=[m_super_init("Lvivo/app/popupcamera/AbsPopupCameraManager;", []), m_singleton_get_instance(PCM)],
    )
    b.add_class("Lvivo/app/proxcali/AbsVivoProxCaliManager;", OBJ, ACC_PUBLIC | ACC_ABSTRACT,
                direct_methods=[m_super_init(OBJ, [])])
    PXM = "Lcom/vivo/framework/proxcali/VivoProxCaliManager;"
    b.add_class(
        PXM, "Lvivo/app/proxcali/AbsVivoProxCaliManager;", ACC_PUBLIC,
        static_fields=[(PXM, "sInstance", ACC_STATIC, None)],
        direct_methods=[m_super_init("Lvivo/app/proxcali/AbsVivoProxCaliManager;", []), m_singleton_get_instance(PXM)],
    )

    dex_bytes = b.build()
    with open(out_path, "wb") as f:
        f.write(dex_bytes)
    print(f"Built VivoCamera compat DEX: {out_path} ({len(dex_bytes)} bytes, {len(b.classes)} classes)")


if __name__ == "__main__":
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "classes15.dex")
    build_compat_dex(out)
