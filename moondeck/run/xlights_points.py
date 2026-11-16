"""Write a Points layout file per controller from an xLights layout: where every light is, in each controller's own wiring order.

xLights keeps the layout in `xlights_rgbeffects.xml` (the models, where they sit and which channels they take) and `xlights_networks.xml` (the controllers). This reads both and writes one `.csv` per controller in the format Points Layout reads: one light per line, `x,y,z`, in the controller's pixel order, with `0` as a fourth field for a pixel no model covers. Copy a controller's file to that device's `/layouts` folder and pick it in a Points layout.

    uv run moondeck/run/xlights_points.py --show "X:/Shows" --out build/points            # every controller
    uv run moondeck/run/xlights_points.py --show "X:/Shows" --controller "Front Door"       # one controller
    uv run moondeck/run/xlights_points.py --show "X:/Shows" --cell 6 --origin shared       # one yard-wide space

Model shapes placed exactly: Custom (2D and 3D, with its rotation), Poly Line (straight segments), and Single Line. Any other model is placed as one point at its position, with a warning, so its lights keep their wiring slots and can be refined later.

Coordinates: xLights x stays x, xLights y (up) becomes y counted downward from the top, as MoonLight's rows run, and xLights z stays z. The device keeps one buffer cell per grid point of a file's box, so each per-controller file takes the finest step, from `--cell` up, whose box fits `--max-cells`. `--origin per-controller` (the default) starts each file at 0,0,0 and writes where that origin sits in the shared space as a comment; `--origin shared` puts every file in one space at one step, for a group of devices that share a canvas.
"""
from __future__ import annotations

import argparse
import math
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

CHANNELS_PER_NODE = {"RGB Nodes": 3, "RGBW Nodes": 4, "Single Color": 1, "Single Color Intensity": 1,
                     "Node Single Color": 1, "3 Channel RGB": 3, "4 Channel RGBW": 4, "Strobes": 1}
MAX_COORD = 32767


@dataclass
class Model:
    """One xLights model: its controller, its first channel there, its channels per light and its lights' world positions in node order."""
    name: str
    controller: str | None
    start: int                      # 0-based channel within the controller
    channels_per_node: int
    points: list[tuple[float, float, float]] = field(default_factory=list)
    approximate: bool = False


def floats(text: str) -> list[float]:
    """The numbers in a comma-separated attribute, empty when there are none."""
    return [float(v) for v in text.split(",") if v.strip()] if text else []


def rotate(p: tuple[float, float, float], rx: float, ry: float, rz: float) -> tuple[float, float, float]:
    """Rotate a point about x, then y, then z, angles in degrees."""
    x, y, z = p
    for axis, deg in (("x", rx), ("y", ry), ("z", rz)):
        if not deg:
            continue
        c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
        if axis == "x":
            y, z = y * c - z * s, y * s + z * c
        elif axis == "y":
            x, z = x * c + z * s, -x * s + z * c
        else:
            x, y = x * c - y * s, x * s + y * c
    return x, y, z


def world(a: dict) -> tuple[float, float, float]:
    return float(a.get("WorldPosX", 0)), float(a.get("WorldPosY", 0)), float(a.get("WorldPosZ", 0))


def scale(a: dict) -> tuple[float, float, float]:
    return float(a.get("ScaleX", 1)), float(a.get("ScaleY", 1)), float(a.get("ScaleZ", 1))


def custom_points(a: dict) -> list[tuple[float, float, float]]:
    """A Custom model's lights: each node's cell (row, column, layer) centered on the model, scaled, rotated and moved to its position. A node over several cells takes the first."""
    w, h, d = int(a.get("CustomWidth", 1)), int(a.get("CustomHeight", 1)), int(a.get("Depth", 1) or 1)
    cells: dict[int, tuple[int, int, int]] = {}
    compressed = a.get("CustomModelCompressed", "")
    if compressed:
        for entry in compressed.split(";"):
            parts = [int(v) for v in entry.split(",") if v.strip()]
            if len(parts) == 4:
                node, row, col, layer = parts
                cells.setdefault(node, (row, col, layer))
    else:   # the uncompressed grid: layers split by '|', rows by ';', cells by ','
        for layer, grid in enumerate(a.get("CustomModel", "").split("|")):
            for row, line in enumerate(grid.split(";")):
                for col, cell in enumerate(line.split(",")):
                    if cell.strip().isdigit():
                        cells.setdefault(int(cell), (row, col, layer))
    sx, sy, sz = scale(a)
    rx, ry, rz = (float(a.get(k, 0) or 0) for k in ("RotateX", "RotateY", "RotateZ"))
    wx, wy, wz = world(a)
    out = []
    for node in range(1, (max(cells) if cells else 0) + 1):
        row, col, layer = cells.get(node, ((h - 1) / 2, (w - 1) / 2, (d - 1) / 2))   # a node with no cell sits at the center
        local = ((col - (w - 1) / 2) * sx, ((h - 1 - row) - (h - 1) / 2) * sy, (layer - (d - 1) / 2) * sz)
        x, y, z = rotate(local, rx, ry, rz)
        out.append((wx + x, wy + y, wz + z))
    return out


def along(path: list[tuple[float, float, float]], counts: list[int]) -> list[tuple[float, float, float]]:
    """Lights spread along a path: `counts` per segment, each light at the center of its share of that segment."""
    out = []
    for (p, q), n in zip(zip(path, path[1:]), counts):
        for i in range(n):
            f = (i + 0.5) / n
            out.append(tuple(p[j] + f * (q[j] - p[j]) for j in range(3)))
    return out


def split_evenly(path: list[tuple[float, float, float]], total: int) -> list[int]:
    """How many of `total` lights each segment gets when they are spread evenly by length."""
    lengths = [math.dist(p, q) for p, q in zip(path, path[1:])]
    whole = sum(lengths) or 1.0
    counts, given, run = [], 0, 0.0
    for length in lengths:
        run += length
        upto = round(total * run / whole)
        counts.append(upto - given)
        given = upto
    return counts


def polyline_points(a: dict) -> list[tuple[float, float, float]]:
    """A Poly Line's lights along its straight segments, honoring per-segment counts where xLights stores them."""
    v = floats(a.get("PointData", ""))
    sx, sy, sz = scale(a)
    wx, wy, wz = world(a)
    path = [(wx + v[i] * sx, wy + v[i + 1] * sy, wz + v[i + 2] * sz) for i in range(0, len(v) - 2, 3)]
    if len(path) < 2:
        return []
    total = int(a.get("NodesPerString", 0)) * int(a.get("PolyStrings", 1) or 1)
    sizes = [int(a[f"PolyLineSegSize{i}"]) for i in range(len(path) - 1) if f"PolyLineSegSize{i}" in a]
    counts = sizes if len(sizes) == len(path) - 1 and sum(sizes) == total else split_evenly(path, total)
    return along(path, counts)


def single_line_points(a: dict) -> list[tuple[float, float, float]]:
    """A Single Line's lights, evenly between its position and its second point."""
    p = world(a)
    q = (p[0] + float(a.get("X2", 0)), p[1] + float(a.get("Y2", 0)), p[2] + float(a.get("Z2", 0)))
    n = int(a.get("parm1", 1) or 1) * int(a.get("parm2", 0) or 0)
    return along([p, q], [n])


SHAPES = {"Custom": custom_points, "Poly Line": polyline_points, "Single Line": single_line_points}


def node_count(a: dict) -> int:
    """How many lights a model of a shape this tool does not place has, from the attributes xLights uses for most models."""
    strings = int(a.get("NumStrings", a.get("parm1", 1)) or 1)
    nodes = int(a.get("NodesPerString", a.get("parm2", 0)) or 0)
    return strings * nodes


def read_controllers(networks: Path) -> dict[str, dict]:
    """Each controller's name, address and first absolute channel, in the order xLights numbers them."""
    out, first = {}, 0
    for c in ET.parse(networks).getroot().iter("Controller"):
        net = c.find("network")
        size = int(net.get("MaxChannels", 0)) if net is not None else 0
        out[c.get("Name")] = {"ip": net.get("ComPort", "") if net is not None else "", "first": first, "size": size}
        first += size
    return out


def locate(start: str, controllers: dict[str, dict], ends: dict[str, tuple[str, int]]) -> tuple[str | None, int]:
    """A StartChannel as (controller, 0-based channel there): `!Controller:N`, `>Model:N` after another model, or an absolute channel."""
    if start.startswith("!"):
        name, ch = start[1:].rsplit(":", 1)
        return (name, int(ch) - 1) if name in controllers else (None, -1)
    if start.startswith(">"):
        prev, ch = start[1:].rsplit(":", 1)
        if prev not in ends:
            return None, -1
        name, end = ends[prev]
        return name, end + int(ch) - 1
    absolute = int(start) - 1
    for name, c in controllers.items():
        if c["first"] <= absolute < c["first"] + c["size"]:
            return name, absolute - c["first"]
    return None, -1


def read_models(effects: Path, controllers: dict[str, dict], warn) -> list[Model]:
    """Every active model with a controller, its lights placed. Models chained with `>` are resolved in file order, repeating until no more resolve."""
    pending = [m.attrib for m in ET.parse(effects).getroot().iter("model") if m.get("Active", "1") != "0"]
    ends: dict[str, tuple[str, int]] = {}
    models: list[Model] = []
    while pending:
        progressed, waiting = False, []
        for a in pending:
            start = a.get("StartChannel", "1")
            ctrl, offset = locate(start, controllers, ends)
            if ctrl is None and start.startswith(">") and start[1:].rsplit(":", 1)[0] not in ends:
                waiting.append(a)
                continue
            progressed = True
            model = place_model(a, ctrl, offset, warn)
            if ctrl is not None:
                ends[model.name] = (ctrl, offset + len(model.points) * model.channels_per_node)
                models.append(model)
        if not progressed:
            for a in waiting:
                warn(f"{a.get('name')}: chained after a model that is not placed; skipped")
            break
        pending = waiting
    return models


def place_model(a: dict, ctrl: str | None, offset: int, warn) -> Model:
    """One model's lights, placed exactly for the shapes this tool knows and as one point otherwise."""
    kind = a.get("DisplayAs", "")
    cpn = CHANNELS_PER_NODE.get(a.get("StringType", "RGB Nodes"), 3)
    model = Model(a.get("name", "?"), ctrl, offset, cpn)
    if kind in SHAPES:
        model.points = SHAPES[kind](a)
        if a.get("Dir", "L") != "L" and kind != "Custom":
            warn(f"{model.name}: starts from the other end in xLights; this tool places it from its first point, so check the order")
    else:
        model.points = [world(a)] * node_count(a)
        model.approximate = True
        if ctrl is not None and model.points:
            warn(f"{model.name}: {kind} is placed as one point at its position; its {len(model.points)} lights keep their wiring slots")
    return model


@dataclass
class Light:
    at: tuple[float, float, float] | None   # None: no model covers this pixel
    model: str = ""


def controller_lights(models: list[Model], warn) -> dict[str, list[Light]]:
    """Each controller's lights in pixel order, a pixel no model covers left empty. A pixel's index is its channel over its model's channels per light."""
    out: dict[str, list[Light]] = {}
    for m in models:
        lights = out.setdefault(m.controller, [])
        first = m.start // m.channels_per_node
        if m.start % m.channels_per_node:
            warn(f"{m.name}: starts mid-pixel at channel {m.start + 1} of {m.controller}; placed from pixel {first}")
        need = first + len(m.points)
        lights.extend(Light(None) for _ in range(need - len(lights)))
        for i, p in enumerate(m.points):
            if lights[first + i].at is not None:
                warn(f"{m.controller} pixel {first + i}: claimed by {lights[first + i].model} and {m.name}; kept the first")
                continue
            lights[first + i] = Light(p, m.name)
    for name, lights in out.items():
        sizes = {m.channels_per_node for m in models if m.controller == name}
        if len(sizes) > 1:
            warn(f"{name}: models mix {sorted(sizes)} channels per light, so pixel indices may not match the device")
    return out


def to_grid(p: tuple[float, float, float], origin: tuple[float, float, float], top: float, cell: float) -> tuple[int, int, int]:
    """A world position as grid steps from the origin, y counted down from `top`."""
    return (round((p[0] - origin[0]) / cell), round((top - p[1]) / cell), round((p[2] - origin[2]) / cell))


def fill_dark(lights: list[Light]) -> list[tuple[float, float, float]]:
    """Each light's position, an uncovered pixel taking the nearest covered one before it, or after it at the start."""
    known = [l.at for l in lights if l.at is not None]
    last = known[0] if known else (0.0, 0.0, 0.0)
    out = []
    for l in lights:
        last = l.at if l.at is not None else last
        out.append(last)
    return out


def write_points(path: Path, name: str, ip: str, lights: list[Light], origin, top, cell, shared_origin) -> tuple[int, int, int]:
    """Write one controller's file; answer its grid box."""
    positions = fill_dark(lights)
    grid = [to_grid(p, origin, top, cell) for p in positions]
    box = tuple(max((g[i] for g in grid), default=-1) + 1 for i in range(3))
    if any(v > MAX_COORD for g in grid for v in g) or any(v < 0 for g in grid for v in g):
        raise ValueError(f"{name}: positions fall outside 0..{MAX_COORD}; use a larger --cell")
    models = []
    for i, l in enumerate(lights):
        if l.at is not None and (not models or models[-1][0] != l.model):
            models.append([l.model, i, i])
        elif l.at is not None:
            models[-1][2] = i
    lines = [f"# {name}{' ' + ip if ip else ''}: {len(lights)} lights from xLights, one grid step = {cell:g} xLights units",
             f"# this file's 0,0,0 is {shared_origin[0]},{shared_origin[1]},{shared_origin[2]} in the shared layout space"]
    lines += [f"# lights {a}-{b}: {m}" for m, a, b in models]
    for g, l in zip(grid, lights):
        lines.append(f"{g[0]},{g[1]},{g[2]}" + ("" if l.at is not None else ",0"))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return box


def safe_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "-", name).strip("-") or "controller"


def grid_box(positions, origin, top, cell) -> tuple[int, int, int]:
    """The grid box a set of positions fills, from 0 to the furthest step on each axis."""
    grid = [to_grid(p, origin, top, cell) for p in positions]
    return tuple(max(g[i] for g in grid) + 1 for i in range(3))


def fitting_cell(positions, origin, top, cell: float, max_cells: int) -> float:
    """The finest grid step, from `cell` up, whose box holds at most `max_cells` cells: the device's buffer is one cell per grid point of the box."""
    while True:
        w, h, d = grid_box(positions, origin, top, cell)
        if w * h * d <= max_cells:
            return cell
        cell *= 1.25


def convert(show: Path, out: Path, cell: float, origin_mode: str, only: str | None, max_cells: int = 16384,
            warn=print) -> dict[str, dict]:
    """Write the files; answer, per controller, its file, light count, grid step and grid box."""
    controllers = read_controllers(show / "xlights_networks.xml")
    models = read_models(show / "xlights_rgbeffects.xml", controllers, warn)
    per = controller_lights(models, warn)
    every = [l.at for lights in per.values() for l in lights if l.at is not None]
    if not every:
        raise ValueError("no model with a controller and lights was found")
    shared = (min(p[0] for p in every), max(p[1] for p in every), min(p[2] for p in every))
    out.mkdir(parents=True, exist_ok=True)
    result = {}
    for name, lights in per.items():
        if only and name != only:
            continue
        mine = [l.at for l in lights if l.at is not None]
        if origin_mode == "shared":
            origin, top, step = (shared[0], 0.0, shared[2]), shared[1], cell
        else:
            origin, top = (min(p[0] for p in mine), 0.0, min(p[2] for p in mine)), max(p[1] for p in mine)
            step = fitting_cell(mine, origin, top, cell, max_cells)
        offset = to_grid((origin[0], top, origin[2]), (shared[0], 0.0, shared[2]), shared[1], step)
        path = out / f"{safe_name(name)}.csv"
        box = write_points(path, name, controllers.get(name, {}).get("ip", ""), lights, origin, top, step, offset)
        if box[0] * box[1] * box[2] > max_cells:
            warn(f"{name}: its grid of {box[0] * box[1] * box[2]} cells passes --max-cells {max_cells}; the device will shrink it")
        result[name] = {"file": path, "lights": len(lights), "cell": step, "box": box}
    return result


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--show", type=Path, required=True, help="the xLights show folder, holding xlights_rgbeffects.xml and xlights_networks.xml")
    ap.add_argument("--out", type=Path, default=Path("build/points"), help="where the .csv files go")
    ap.add_argument("--cell", type=float, default=1.0, help="the finest grid step, in xLights units (default 1)")
    ap.add_argument("--max-cells", type=int, default=16384,
                    help="the largest grid box a file may fill; per-controller files coarsen their step until they fit (default 16384, 128x128)")
    ap.add_argument("--origin", choices=("per-controller", "shared"), default="per-controller")
    ap.add_argument("--controller", help="write only this controller's file")
    args = ap.parse_args(argv)
    try:
        result = convert(args.show, args.out, args.cell, args.origin, args.controller, args.max_cells,
                         warn=lambda m: print(f"warning: {m}", file=sys.stderr))
    except (OSError, ValueError, ET.ParseError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    for name, r in result.items():
        w, h, d = r["box"]
        print(f"{name}: {r['lights']} lights, step {r['cell']:.3g}, grid {w}x{h}x{d} ({w * h * d} cells) -> {r['file']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
