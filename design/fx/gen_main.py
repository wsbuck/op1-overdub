#!/usr/bin/env python3
"""Main concept: the two-pedal scene with the wah drawn from the 3D model (gen_rot.py variant A), turned 45 deg clockwise
on the floor (toe back-left toward the box, heel front-right). Runs gen_pedals.py's main block with that wah."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_pedals as gp
import gen_rot as r

gp.asset = r.asset_a
gp.runtime_state = r.make_runtime("a")
gp.HEADER = r.HDR_A.replace("pedals_a.svg", "pedals.svg").replace(
    "WAH (green; VARIANT A replaces the next lines, see below; static group \"wah\" = translate(184 0) * scale(1.0) about (4, 136)), oblique view: "
    "side faces true shape, depth vector D = (11, -11).", "WAH (green), drawn from a 3D model (below).").replace(
    'VARIANT A "on the floor": the wah yawed 45 deg CCW seen from above', 'the wah turned 45 deg CLOCKWISE on the floor as seen on screen (yaw 135 deg:\n  toe back-left toward the box, heel front-right)').replace(
    "Input jack on the side\n  near the toe (right), output near the heel (left). Visible faces: side, heel end,",
    "Input jack on the visible (far, Y = 22) side\n  near the heel (front-right, cable to the right edge), output near the toe (back-left, patch to the box). Visible faces: side (Y = 22), heel end,").replace(
    "tside (-5,0,33) (111,0,33) (-5,0,25)", "tside (-5,22,33) (111,22,33) (-5,22,25)")
src = open(os.path.join(gp.OUT, "gen_pedals.py")).read()
code = src[src.index('if __name__ == "__main__":'):].replace('if __name__ == "__main__":', "if True:", 1)
code = code.replace("The wah's treadle", "the wah's treadle")
exec(compile(code, "gen_pedals_main", "exec"), gp.__dict__)
