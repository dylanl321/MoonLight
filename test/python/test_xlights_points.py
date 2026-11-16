# /// script
# dependencies = ["pytest"]
# ///
"""xLights to Points layout: each controller's lights in its own wiring order, at grid positions Points Layout reads."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "moondeck" / "run"))

import xlights_points  # noqa: E402

NETWORKS = """<?xml version="1.0"?>
<Networks>
  <Controller Name="Porch"><network ComPort="10.0.0.5" MaxChannels="300"/></Controller>
  <Controller Name="Yard"><network ComPort="10.0.0.6" MaxChannels="400"/></Controller>
</Networks>
"""


def show(tmp_path, models: str) -> Path:
    (tmp_path / "xlights_networks.xml").write_text(NETWORKS)
    (tmp_path / "xlights_rgbeffects.xml").write_text(f'<?xml version="1.0"?>\n<xrgb><models>{models}</models></xrgb>\n')
    return tmp_path


def lights(path: Path) -> list[list[int]]:
    """The light lines of a written file, as numbers."""
    return [[int(v) for v in line.split(",")] for line in path.read_text().splitlines() if line and not line.startswith("#")]


def test_a_poly_line_spreads_its_lights_evenly_along_its_segments(tmp_path):
    """Ten lights over a 100-unit line sit at the center of each tenth, so the first is 5 units in."""
    s = show(tmp_path, '<model name="Rail" DisplayAs="Poly Line" StringType="RGB Nodes" NodesPerString="10" '
                       'PointData="0,0,0,100,0,0" StartChannel="!Porch:1" WorldPosX="0" WorldPosY="0" WorldPosZ="0"/>')
    r = xlights_points.convert(s, tmp_path / "out", 1.0, "per-controller", None, warn=lambda m: None)
    pts = lights(r["Porch"]["file"])
    assert len(pts) == 10
    assert [p[0] for p in pts] == [0, 10, 20, 30, 40, 50, 60, 70, 80, 90]   # 5, 15, ... shifted to start at 0


def test_lights_follow_the_controller_channel_order_not_the_file_order(tmp_path):
    """A model starting at channel 31 of an RGB controller owns pixels 10 onward, and the pixels before it are dark."""
    s = show(tmp_path,
             '<model name="Late" DisplayAs="Poly Line" StringType="RGB Nodes" NodesPerString="2" PointData="0,0,0,10,0,0" '
             'StartChannel="!Porch:31" WorldPosX="50" WorldPosY="0" WorldPosZ="0"/>'
             '<model name="Early" DisplayAs="Poly Line" StringType="RGB Nodes" NodesPerString="3" PointData="0,0,0,30,0,0" '
             'StartChannel="!Porch:1" WorldPosX="0" WorldPosY="0" WorldPosZ="0"/>')
    r = xlights_points.convert(s, tmp_path / "out", 1.0, "per-controller", None, warn=lambda m: None)
    pts = lights(r["Porch"]["file"])
    assert len(pts) == 12
    assert all(len(p) == 3 for p in pts[:3])            # Early lights pixels 0-2
    assert all(p[3] == 0 for p in pts[3:10])            # nothing covers 3-9: dark
    assert pts[10][0] > pts[2][0]                       # Late sits further along x
    text = r["Porch"]["file"].read_text()
    assert "# lights 0-2: Early" in text and "# lights 10-11: Late" in text


def test_an_rgbw_model_takes_four_channels_per_light(tmp_path):
    """Channel 41 of an RGBW string is pixel 10."""
    s = show(tmp_path, '<model name="Pucks" DisplayAs="Poly Line" StringType="RGBW Nodes" NodesPerString="2" '
                       'PointData="0,0,0,10,0,0" StartChannel="!Yard:41" WorldPosX="0" WorldPosY="0" WorldPosZ="0"/>')
    r = xlights_points.convert(s, tmp_path / "out", 1.0, "per-controller", None, warn=lambda m: None)
    assert r["Yard"]["lights"] == 12


def test_a_custom_model_places_each_node_at_its_cell_with_y_counted_down(tmp_path):
    """A 3x2 custom grid: node 1 top left, node 2 bottom right; the top row is y 0 in the file."""
    s = show(tmp_path, '<model name="Box" DisplayAs="Custom" StringType="RGB Nodes" CustomWidth="3" CustomHeight="2" Depth="1" '
                       'CustomModelCompressed="1,0,0,0;2,1,2,0" ScaleX="10" ScaleY="10" ScaleZ="10" '
                       'StartChannel="!Porch:1" WorldPosX="0" WorldPosY="0" WorldPosZ="0"/>')
    r = xlights_points.convert(s, tmp_path / "out", 1.0, "per-controller", None, warn=lambda m: None)
    pts = lights(r["Porch"]["file"])
    assert pts[0] == [0, 0, 0]
    assert pts[1] == [20, 10, 0]


def test_a_model_chained_after_another_starts_where_that_one_ends(tmp_path):
    """`>First:1` places Second's first light right after First's last."""
    s = show(tmp_path,
             '<model name="Second" DisplayAs="Poly Line" StringType="RGB Nodes" NodesPerString="2" PointData="0,0,0,10,0,0" '
             'StartChannel=">First:1" WorldPosX="20" WorldPosY="0" WorldPosZ="0"/>'
             '<model name="First" DisplayAs="Poly Line" StringType="RGB Nodes" NodesPerString="3" PointData="0,0,0,10,0,0" '
             'StartChannel="!Porch:1" WorldPosX="0" WorldPosY="0" WorldPosZ="0"/>')
    r = xlights_points.convert(s, tmp_path / "out", 1.0, "per-controller", None, warn=lambda m: None)
    assert r["Porch"]["lights"] == 5
    assert "# lights 3-4: Second" in r["Porch"]["file"].read_text()


def test_an_unknown_shape_keeps_its_wiring_slots_at_one_point_and_says_so(tmp_path):
    warnings = []
    s = show(tmp_path, '<model name="Bush" DisplayAs="Tree" StringType="RGB Nodes" NumStrings="1" NodesPerString="4" '
                       'StartChannel="!Porch:1" WorldPosX="7" WorldPosY="0" WorldPosZ="0"/>')
    r = xlights_points.convert(s, tmp_path / "out", 1.0, "per-controller", None, warn=warnings.append)
    pts = lights(r["Porch"]["file"])
    assert len(pts) == 4 and len({tuple(p) for p in pts}) == 1
    assert any("Bush" in w and "one point" in w for w in warnings)


def test_a_large_layout_coarsens_its_step_until_the_box_fits(tmp_path):
    """A 1000-unit line at step 1 would be 1000 cells wide; with a 100-cell budget the step grows until it fits."""
    s = show(tmp_path, '<model name="Long" DisplayAs="Poly Line" StringType="RGB Nodes" NodesPerString="50" '
                       'PointData="0,0,0,1000,0,0" StartChannel="!Porch:1" WorldPosX="0" WorldPosY="0" WorldPosZ="0"/>')
    r = xlights_points.convert(s, tmp_path / "out", 1.0, "per-controller", None, max_cells=100, warn=lambda m: None)
    w, h, d = r["Porch"]["box"]
    assert w * h * d <= 100
    assert r["Porch"]["cell"] > 1.0


def test_shared_origin_puts_two_controllers_in_one_space(tmp_path):
    """With a shared origin the second controller's lights keep their distance from the first's."""
    s = show(tmp_path,
             '<model name="A" DisplayAs="Poly Line" StringType="RGB Nodes" NodesPerString="1" PointData="0,0,0,2,0,0" '
             'StartChannel="!Porch:1" WorldPosX="0" WorldPosY="0" WorldPosZ="0"/>'
             '<model name="B" DisplayAs="Poly Line" StringType="RGB Nodes" NodesPerString="1" PointData="0,0,0,2,0,0" '
             'StartChannel="!Yard:1" WorldPosX="100" WorldPosY="0" WorldPosZ="0"/>')
    r = xlights_points.convert(s, tmp_path / "out", 1.0, "shared", None, warn=lambda m: None)
    assert lights(r["Yard"]["file"])[0][0] - lights(r["Porch"]["file"])[0][0] == 100


def test_every_written_line_is_one_points_layout_accepts(tmp_path):
    """Three or four whole numbers, coordinates 0 to 32767, a fourth field only ever 0."""
    s = show(tmp_path, '<model name="Rail" DisplayAs="Poly Line" StringType="RGB Nodes" NodesPerString="5" '
                       'PointData="0,0,0,40,30,20" StartChannel="!Porch:10" WorldPosX="-500" WorldPosY="-50" WorldPosZ="-5"/>')
    r = xlights_points.convert(s, tmp_path / "out", 1.0, "per-controller", None, warn=lambda m: None)
    for p in lights(r["Porch"]["file"]):
        assert len(p) in (3, 4)
        assert all(0 <= v <= 32767 for v in p[:3])
        assert len(p) == 3 or p[3] == 0
