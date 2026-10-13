#!/usr/bin/env python3
"""Wah rotated 45 deg counter-clockwise, two readings:
  A  "on the floor": yawed 45 deg CCW seen from above, redrawn from a 3D model in the same oblique floor projection;
  B  "on the screen": the current wah artwork rotated 45 deg CCW in the screen plane (static parent matrix).
Reuses gen_pedals.py (distortion box, runtime, overlay, composite)."""
import json, math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_pedals as gp
from gen_pedals import N, C, f, rrect, poly_d, circ, mul, rot, tr, apply, DX, JY

OUT = gp.OUT
BASE_ASSET = gp.asset

# ------------------------------------------------------------------ variant A: 3D model, oblique projection
# world: X along the wah (heel 0 -> toe 106), Y across (0 = near side, 22 = far side), Z up. Old view: screen =
# (X + Y/2, -Z - Y/2) (the depth vector D = (11, -11) for the 22-unit width). Yaw 45 deg CCW from above about the
# body centre (53, 11): the toe swings away (back-right), the heel comes forward (front-left).
YAW = math.radians(135)               # seen on screen: the wah turned 45 deg clockwise, toe back-left, heel front-right
SY = 22                               # the visible side face is the far side (Y = 22) at this yaw
SA = 1.12
OA = (52, 148)                         # screen offset (fitted below)
AX = (53.0, 25.0)                      # treadle axle (X, Z), along Y


def P3(X, Y, Z):
    x, y = X - 53, Y - 11
    Xr = x * math.cos(YAW) - y * math.sin(YAW)
    Yr = x * math.sin(YAW) + y * math.cos(YAW)
    return (OA[0] + SA * (Xr + 0.5 * Yr), OA[1] + SA * (-Z - 0.5 * Yr))


def _fit():
    global OA, SA
    OA, SA = (0, 0), 1.0
    P = [P3(X, Y, Z) for X in (-6, 112) for Y in (-3, 25) for Z in (0, 33 + 10)]
    x0, x1 = min(p[0] for p in P), max(p[0] for p in P)
    y0, y1 = min(p[1] for p in P), max(p[1] for p in P)
    SA = min(124 / (x1 - x0), 1.3)
    OA = (188 - SA * x0, 136 - SA * y1)


_fit()


def rock(X, Y, Z, th):
    c, s = math.cos(th), math.sin(th)
    return (AX[0] + (X - AX[0]) * c + (Z - AX[1]) * s, Y, AX[1] + (Z - AX[1]) * c - (X - AX[0]) * s)


def fit3(src, dst):
    """affine matrix (a b c d e f) mapping 3 screen points src -> dst"""
    (x1, y1), (x2, y2), (x3, y3) = src
    (u1, v1), (u2, v2), (u3, v3) = dst
    det = (x2 - x1) * (y3 - y1) - (x3 - x1) * (y2 - y1)
    a = ((u2 - u1) * (y3 - y1) - (u3 - u1) * (y2 - y1)) / det
    c = ((u3 - u1) * (x2 - x1) - (u2 - u1) * (x3 - x1)) / det
    b = ((v2 - v1) * (y3 - y1) - (v3 - v1) * (y2 - y1)) / det
    d = ((v3 - v1) * (x2 - x1) - (v2 - v1) * (x3 - x1)) / det
    return (a, b, c, d, u1 - a * x1 - c * y1, v1 - b * x1 - d * y1)


# reference points (3 per treadle face, in the face's plane)
REF = {"ttop": [(-5, 0, 33), (111, 0, 33), (-5, 22, 33)],
       "tside": [(-5, SY, 33), (111, SY, 33), (-5, SY, 25)],
       "tend": [(-5, 0, 33), (-5, 22, 33), (-5, 0, 25)]}


def treadle_a(a):
    th = math.radians(gp.MAXDEG * a)
    return {g: fit3([P3(*p) for p in R], [P3(*rock(*p, th)) for p in R]) for g, R in REF.items()}


def wah_a():
    sw = dict(stroke_width="1", stroke_linejoin="round")
    Q = lambda L: poly_d([P3(*p) for p in L])
    side = [(0, SY, 0), (106, SY, 0), (106, SY, 14), (53, SY, 21), (0, SY, 14)]
    heel = [(0, 0, 0), (0, 22, 0), (0, 22, 14), (0, 0, 14)]
    roofL = [(0, 0, 14), (53, 0, 21), (53, 22, 21), (0, 22, 14)]
    roofR = [(53, 0, 21), (106, 0, 14), (106, 22, 14), (53, 22, 21)]
    shadow = [(-6, -3, 0), (112, -3, 0), (112, 25, 0), (-6, 25, 0)]
    brk = [(44, SY + 0.5, 19), (62, SY + 0.5, 19), (57, SY + 0.5, 26), (49, SY + 0.5, 26)]
    top = [(-5, 0, 33), (111, 0, 33), (111, 22, 33), (-5, 22, 33)]
    tside = [(-3, SY, 25), (108, SY, 25), (111, SY, 28), (111, SY, 33), (-5, SY, 33), (-5, SY, 27)]
    tend = [(-5, 0, 33), (-5, 22, 33), (-5, 22, 27), (-5, 0, 27)]
    ribs = " ".join(f"M{f(P3(x, 1.5, 33)[0])} {f(P3(x, 1.5, 33)[1])} L{f(P3(x, 20.5, 33)[0])} {f(P3(x, 20.5, 33)[1])}"
                    for x in range(2, 108, 8))
    s0, s1 = P3(1, SY + 0.2, 30.6), P3(104, SY + 0.2, 30.6)
    ji, jo = P3(9, SY + 0.3, 7), P3(97, SY + 0.3, 7)    # input near the heel (front-right), output near the toe (back-left)
    ax = P3(53, SY + 0.6, 28.5)
    ci = (ji[0] - 12, ji[1] + 2)
    return N("g", "wah", [
        N("path", "wshadow", d=Q(shadow), fill=C["offblue"]),
        N("path", "wroofL", d=Q(roofL), fill=C["grey"], stroke=C["grey"], **sw),
        N("path", "wroofR", d=Q(roofR), fill=C["grey"], stroke=C["grey"], **sw),
        N("path", "wend", d=Q(heel), fill=C["offgrey"], stroke=C["grey"], **sw),
        N("path", "wside", d=Q(side), fill=C["offgrey"], stroke=C["grey"], **sw),
        N("path", "wbrk", d=Q(brk), fill=C["purplegrey"], stroke=C["grey"], stroke_width="0.8"),
        N("g", "ttop", [N("path", "rubber", d=Q(top), fill=C["black"], stroke=C["purplegrey"], stroke_width="1", stroke_linejoin="round"),
                        N("path", "ribs", d=ribs, fill="none", stroke=C["green"], stroke_width="1.6", stroke_linecap="round")]),
        N("g", "tend", [N("path", "tendf", d=Q(tend), fill=C["grey"], stroke=C["lpg"], stroke_width="0.8", stroke_linejoin="round")]),
        N("g", "tside", [N("path", "chrome", d=Q(tside), fill=C["purplegrey"], stroke=C["lpg"], stroke_width="1", stroke_linejoin="round"),
                         N("line", "shine", x1=f(s0[0]), y1=f(s0[1]), x2=f(s1[0]), y2=f(s1[1]), stroke=C["white"],
                           stroke_width="1.2", stroke_linecap="round")]),
        circ(ax[0], ax[1], 3.6, "axle", fill=C["green"], stroke=C["black"], stroke_width="1"),
        N("path", "cableI", d=f"M{f(ji[0])} {f(ji[1] + 4)} C{f(ji[0])} {f(ji[1] + 16)} 316 {f(ji[1] + 2)} 330 {f(ji[1] - 6)}", fill="none",
          stroke=C["offgrey"], stroke_width="3", stroke_linecap="round"),
        circ(ji[0], ji[1], 2.6, "wjackI", fill=C["black"], stroke=C["purplegrey"], stroke_width="1"),
        circ(jo[0], jo[1], 3.4, "wjackO", fill=C["grey"], stroke=C["purplegrey"], stroke_width="1"),
        N("path", "plugW", d=rrect(ji[0] - 2.4, ji[1] - 2.4, ji[0] + 2.4, ji[1] + 4.5, 1.2), fill=C["grey"], stroke=C["purplegrey"],
          stroke_width="0.8"),
    ]), jo


def asset_a():
    a = BASE_ASSET()
    w, wo = wah_a()
    a.ch = [w, gp.patch_path(wo), a.ch[2]]          # patch leaves the near (heel) side toward the box
    return a


# ------------------------------------------------------------------ variant B: screen-plane rotation of the current art
SB = 1.0
TB_ = (250, 74)                        # where the wah's visual centre goes


def fit_b():
    cb = apply(gp.wah_matrix(), (70, 114))      # the current wah's visual centre on screen
    return mul(tr(TB_[0] - cb[0], TB_[1] - cb[1]), mul(gp.scale_about(SB, SB, *cb), rot(-45, *cb)))


def asset_b():
    a = BASE_ASSET()
    w = a.ch[1]
    M = mul(fit_b(), gp.wah_matrix())
    w.a["transform"] = "matrix(" + " ".join(f"{v:.4f}" for v in M) + ")"
    a.ch[0] = gp.patch_path(apply(M, gp.WJI))
    return a


HDR_A = gp.HEADER.replace("pedals.svg", "pedals_a.svg").replace("WAH (green;", "WAH (green; VARIANT A replaces the next lines, see below;").replace(
    """  tside          matrix = rotate(theta) about the axle (63, 111)
  ttop           matrix = the same rotation for the treadle's long edge, depth kept: linear part [[c, c-1], [s, s+1]]
                 (columns), translation so that (63, 103) -> rotate(theta)(63, 103); c, s = cos, sin(theta)""",
    f"""  VARIANT A "on the floor": the wah yawed 45 deg CCW seen from above, drawn from a 3D model (X along heel 0 -> toe 106,
  Y across 0..22, Z up; roof Z 14/21, treadle Z 25..33, axle (X 53, Z 25) along Y), rotated about (53, 11), projected
  obliquely: screen = ({OA[0]:.2f}, {OA[1]:.2f}) + {SA:.4f} * (X' + Y'/2, -Z - Y'/2). No static wah matrix. Input jack on the side
  near the toe (right), output near the heel (left). Visible faces: side, heel end, roofs, treadle top / side / heel end.
  rock R3(theta) = rotation about the axle line in the X-Z plane (toe down for theta > 0). Each treadle face is planar, so its
  screen image under the rock is affine: M_face = P o R3 o P_face^-1, evaluated as the affine map taking 3 reference points'
  rest screen positions to their rocked ones (a 2x2 solve per face, or a small table over theta):
  ttop  refs (X,Y,Z) = (-5,0,33) (111,0,33) (-5,22,33);  tside (-5,0,33) (111,0,33) (-5,0,25);  tend (-5,0,33) (-5,22,33) (-5,0,25)""")


def hdr_b():
    M = mul(fit_b(), gp.wah_matrix())
    return gp.HEADER.replace("pedals.svg", "pedals_b.svg").replace(
        "WAH (green; static group \"wah\" = translate(184 0) * scale(1.0) about (4, 136)),",
        "WAH (green; VARIANT B \"on the screen\": static group \"wah\" = R(-45 deg CCW) about the wah's centre, moved to (250, 74),\n"
        "  composed with the base placement: matrix(" + " ".join(f"{v:.4f}" for v in M) + "); inside it everything is unchanged,\n"
        "  so tside / ttop (local matrices) still rock about the treadle's own axle and the parent's rotation composes on top),")


FADE_EXTRA = {"tendf": ("grey", "lpg")}


def make_runtime(variant):
    base = gp.runtime_state

    def rs(k, t):
        S = base(k, t)
        if variant == "a":
            for g, M in treadle_a(gp.wah_amount(k[1])).items():
                S[g] = dict(m=M)
            v = 0.15 + 0.85 * k[3] / 32767
            S["tendf"] = dict(fill=gp.lerp_col("grey", v), stroke=gp.lerp_col("lpg", v))
        return S
    return rs


if __name__ == "__main__":
    os.makedirs(os.path.join(OUT, "frames"), exist_ok=True)
    CV = gp.CV
    states = [("heel", (14000, 0, CV[3], 32767), "CLIP, drive 43 %, green full LEFT: heel down (low-pass)"),
              ("level", (14000, 16384, CV[3], 32767), "CLIP, drive 43 %, green MIDDLE: treadle level (flat)"),
              ("toe", (14000, 32767, CV[3], 32767), "CLIP, drive 43 %, green full RIGHT: toe down (focus)"),
              ("muff", (20000, 22000, CV[1], 32767), "MUFF (silver, red rim), drive 61 %, a little toe")]
    idx = json.load(open(os.path.join(OUT, "index.json")))
    idx = [c for c in idx if c["concept"] not in ("rotA", "rotB")]
    rs0 = gp.runtime_state
    for var, mk, hdr in (("a", asset_a, HDR_A), ("b", asset_b, None)):
        hdr = hdr or hdr_b()
        gp.runtime_state = rs0
        gp.runtime_state = make_runtime(var)
        gp.asset = mk
        open(os.path.join(OUT, f"pedals_{var}.svg"), "w").write('<?xml version="1.0" encoding="utf-8"?>\n<!--' + hdr.replace("--", "-") +
                                                                 '-->\n' + gp.render(mk()))
        info = {n: gp.composite(f"rot{var}_{n}", k) for n, k, _ in states}
        seq = []
        for j in range(48):
            g = int(32767 * (0.5 - 0.5 * math.cos(2 * math.pi * j / 48)))
            nm = f"frames/r{var}_{j:02d}"
            gp.composite(nm, (13000, g, CV[3], 32767), t=j * 0.06, w=640)
            seq.append(os.path.join(OUT, nm + ".png"))
        gp.gif(f"pedal_wah_{var}.gif", seq)
        vis = [v[0] for v in info.values()]
        idx.append(dict(
            concept=f"rot{var.upper()}",
            title="Two Pedals, wah turned 45 deg CCW " + ("on the floor (yaw)" if var == "a" else "on the screen (2D)"),
            summary=("The wah yawed 45 deg counter-clockwise on the floor (toe away to the back-right, heel forward), redrawn from a 3D model in the same oblique view; the treadle still rocks about its own axle (per-face affine matrices)."
                     if var == "a" else
                     "The current wah artwork rotated 45 deg counter-clockwise in the screen plane by one static parent matrix, scaled 0.88 to fit beside the distortion box; the treadle's local rock matrix composes under it."),
            route="mixed",
            knobs=idx[0]["knobs"],
            animation=idx[0]["animation"],
            cost=f"SVG: {gp.count(mk())} elements ({min(vis)}-{max(vis)} visible); " + (
                "3 treadle matrices (ttop, tside, tend: 3-point affine fits) + the distortion box's matrices" if var == "a"
                else "same runtime matrices as hq (tside, ttop local) + 1 static parent matrix") + "; overlay: the mode word only",
            states=[dict(file=f"hq/rot{var}_{n}.svg", png=f"hq/rot{var}_{n}.png", caption=c) for n, _, c in states]
            + [dict(file=f"hq/pedal_wah_{var}.gif", caption="green left -> right -> left (CLIP, drive 40 %)")]))
        print(var, info, gp.count(mk()))
    json.dump(idx, open(os.path.join(OUT, "index.json"), "w"), indent=2)
