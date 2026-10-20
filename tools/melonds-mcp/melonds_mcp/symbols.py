import bisect
import re
from dataclasses import dataclass, field
from pathlib import Path



SYMBOL_LINE = re.compile(r"^(\S+)\s+kind:(\w+)(?:\(([^)]*)\))?\s+addr:(0x[0-9a-fA-F]+)")
SAMPLE_WINDOWS = 8
WINDOW = 16


@dataclass
class Symbol:
    name: str
    addr: int
    size: int
    kind: str
    thumb: bool
    module: str


@dataclass
class Module:
    name: str
    overlay: int | None
    base: int = 0
    size: int = 0
    image: bytes = b""
    symbols: list = field(default_factory=list)
    starts: list = field(default_factory=list)

    def index(self):
        self.symbols.sort(key=lambda s: s.addr)
        self.starts = [s.addr for s in self.symbols]
        if not self.size and self.symbols:
            last = self.symbols[-1]
            self.base = self.base or self.symbols[0].addr
            self.size = last.addr + max(last.size, 1) - self.base

    def find(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        if i < 0:
            return None
        s = self.symbols[i]
        if addr < s.addr + max(s.size, 1):
            return s
        return None

    def covers(self, addr):
        return self.base <= addr < self.base + self.size


def _parse_symbols(path, module):
    out = []
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        m = SYMBOL_LINE.match(line)
        if not m:
            continue
        name, kind, args, addr = m.groups()
        size = 0
        thumb = False
        for part in (args or "").split(","):
            part = part.strip()
            if part.startswith("size="):
                size = int(part[5:], 0)
            elif part == "thumb":
                thumb = True
            elif part.startswith("byte[") or part.startswith("word[") or part.startswith("short["):
                count = int(part[part.index("[") + 1:part.index("]")] or "1", 0)
                size = count * {"b": 1, "s": 2, "w": 4}[part[0]]
            elif part in ("byte", "short", "word"):
                size = {"byte": 1, "short": 2, "word": 4}[part]
        out.append(Symbol(name, int(addr, 16), size, kind, thumb, module))
    return out


class SymbolTable:
    def __init__(self):
        self.static = []
        self.overlays = {}
        self.by_name = {}
        self.source = None
        self._loaded_cache = (None, None)

    def load(self, path):
        path = Path(path)
        if path.is_dir():
            path = path / "config.yaml"
        if path.suffix in (".yaml", ".yml"):
            self._load_dsd(path)
        else:
            self._load_plain(path)
        self.source = str(path)
        self.by_name = {}
        for mod in self.static + list(self.overlays.values()):
            mod.index()
            for s in mod.symbols:
                self.by_name.setdefault(s.name, []).append(s)
        self._loaded_cache = (None, None)
        return self.summary()

    def summary(self):
        return {
            "source": self.source,
            "modules": [m.name for m in self.static],
            "overlays": len(self.overlays),
            "symbols": sum(len(m.symbols) for m in self.static + list(self.overlays.values())),
        }

    def _load_dsd(self, config_path):
        import yaml
        cfg = yaml.safe_load(config_path.read_text())
        root = config_path.parent
        self.static, self.overlays = [], {}

        main = Module("main", None)
        main.symbols = _parse_symbols(root / cfg["main_module"]["symbols"], "main")
        self.static.append(main)
        for auto in cfg.get("autoloads", []):
            mod = Module(auto["name"], None)
            mod.symbols = _parse_symbols(root / auto["symbols"], auto["name"])
            self.static.append(mod)

        table = {}
        rom_cfg = cfg.get("rom_config")
        if rom_cfg:
            rom_dir = (root / rom_cfg).parent
            table_path = rom_dir / "arm9_overlays" / "overlays.yaml"
            if table_path.exists():
                for ov in yaml.safe_load(table_path.read_text())["overlays"]:
                    table[ov["id"]] = (ov, rom_dir / "arm9_overlays" / ov["file_name"])

        for ov in cfg.get("overlays", []):
            mod = Module(ov["name"], ov["id"])
            mod.symbols = _parse_symbols(root / ov["symbols"], ov["name"])
            if ov["id"] in table:
                info, image = table[ov["id"]]
                mod.base = info["base_address"]
                mod.size = info["code_size"] + info.get("bss_size", 0)
                if image.exists():
                    mod.image = image.read_bytes()
            self.overlays[ov["id"]] = mod

    def _load_plain(self, path):
        mod = Module(path.stem, None)
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            parts = line.split()
            if len(parts) < 2:
                continue
            try:
                addr = int(parts[0], 16)
            except ValueError:
                continue
            size = int(parts[2], 0) if len(parts) > 2 else 0
            mod.symbols.append(Symbol(parts[1], addr & ~1, size, "function", bool(addr & 1), mod.name))
        self.static, self.overlays = [mod], {}

    def loaded_overlays(self, console):
        key = (console.id, console.frames)
        if self._loaded_cache[0] == key:
            return self._loaded_cache[1]
        loaded = []
        for ov_id, mod in self.overlays.items():
            if not mod.image:
                continue
            code = min(len(mod.image), mod.size)
            hits = 0
            for i in range(SAMPLE_WINDOWS):
                off = (code * (2 * i + 1) // (2 * SAMPLE_WINDOWS)) & ~3
                want = mod.image[off:off + WINDOW]
                if console.read(mod.base + off, len(want)) == want:
                    hits += 1
            if hits * 4 >= SAMPLE_WINDOWS * 3:
                loaded.append(ov_id)
        self._loaded_cache = (key, loaded)
        return loaded

    def lookup(self, addr, console=None):
        addr &= ~1
        for mod in self.static:
            if s := mod.find(addr):
                return s
        candidates = self.loaded_overlays(console) if console else list(self.overlays)
        for ov_id in candidates:
            if s := self.overlays[ov_id].find(addr):
                return s
        return None

    def describe(self, addr, console=None):
        s = self.lookup(addr, console)
        if not s:
            return f"{addr:08x}"
        off = (addr & ~1) - s.addr
        where = f" [{s.module}]" if s.module.startswith("ov") else ""
        return f"{s.name}+0x{off:x}{where}" if off else f"{s.name}{where}"

    def resolve(self, name, console=None):
        syms = self.by_name.get(name)
        if not syms:
            return None
        if len(syms) == 1 or console is None:
            return syms[0]
        loaded = {f"ov{i:03d}" for i in self.loaded_overlays(console)}
        for s in syms:
            if not s.module.startswith("ov") or s.module in loaded:
                return s
        return syms[0]
