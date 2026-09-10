"""Schematics for the project writeups, drawn with schemdraw.
Run: .venv-diagrams/bin/python scripts/diagrams/schem.py [outdir]
"""
import sys, os
import schemdraw
import schemdraw.elements as elm

OUT = sys.argv[1] if len(sys.argv) > 1 else 'public/diagrams'
os.makedirs(OUT, exist_ok=True)
INK = '#1c1a17'; SIG = '#2b5c8a'; PWR = '#b23a2c'; MAINS = '#c26a12'; MUTED = '#6b6559'
FS = 9.5

def box(label, size, left=(), right=(), top=(), bottom=(), spacing=1.0, **kw):
    """Module box. Pin lists are given TOP to BOTTOM (or left to right); schemdraw places bottom-up, so reverse."""
    pins = []
    for side, lst in (('L', left), ('R', right), ('T', top), ('B', bottom)):
        seq = list(lst)[::-1] if side in ('L', 'R') else list(lst)
        for name in seq:
            pins.append(elm.IcPin(name=name, side=side, lblsize=(9 if side in ('T', 'B') else None)))
    ic = elm.Ic(pins=pins, size=size, leadlen=0.6, lblofst=0.12, pinspacing=spacing, **kw)
    return ic.right().label(label, 'top', fontsize=10.5, ofst=0.2)

def P(ic, name):
    return getattr(ic, name)

def line(d, *pts, color=INK):
    for a, b in zip(pts, pts[1:]):
        d.add(elm.Line().at(a).to(b).color(color))

def dot(d, p):
    d.add(elm.Dot(radius=0.07).at(p))

def note(d, xy, text):
    d.add(elm.Label().at(xy).label(text, fontsize=8.5, halign='left', valign='top', color=MUTED))

def new():
    d = schemdraw.Drawing(show=False)
    d.config(fontsize=FS, lw=1.3, unit=2, color=INK, font='Helvetica', margin=0.3)
    return d

def gate():
    d = new()
    esp = d.add(box('ESP32 DevKit  (ESPHome)', (3.6, 5.2), left=['VIN', 'GND'], right=['GPIO26', 'GPIO27', 'GPIO14', 'GND'], spacing=1.0).at((0, 0)))
    rly = d.add(box('Relay module, 5 V coil', (3.2, 3.2), left=['VCC', 'GND', 'IN'], right=['COM', 'NO'], spacing=0.9).at((7.8, 4.6)))
    top = d.add(box('TOPENS gate control board', (3.6, 5.2), left=['4  O/S/C', '5  COM', '15  MOTOR1', '16  MOTOR1'], spacing=1.0).at((15.0, 0.6)))
    o1 = d.add(elm.Optocoupler().reverse().at((9.2, 2.2)).label('U1  PC817', 'top', fontsize=9, ofst=0.15))
    o2 = d.add(elm.Optocoupler().reverse().at((9.2, -1.3)).label('U2  PC817', 'top', fontsize=9, ofst=0.15))
    # power symbols
    d.add(elm.Vdd().at(P(esp, 'VIN')).color(PWR).label('+5V', fontsize=9, color=PWR))
    d.add(elm.Ground().at(P(esp, 'inL1')))
    d.add(elm.Vdd().at(P(rly, 'VCC')).color(PWR).label('+5V', fontsize=9, color=PWR))
    d.add(elm.Ground().at(P(rly, 'GND')))
    d.add(elm.Ground().at(o1.emitter))
    d.add(elm.Ground().at(o2.emitter))
    d.add(elm.Ground().at(P(esp, 'inR1')))
    # relay drive
    g26 = P(esp, 'GPIO26'); rin = P(rly, 'IN')
    line(d, g26, (6.0, g26.y), (6.0, rin.y), rin, color=SIG)
    d.add(elm.Label().at((6.1, (g26.y + rin.y) / 2)).label('500 ms pulse', fontsize=8.5, halign='left', color=SIG))
    # relay contacts across O/S/C and COM
    com, no = P(rly, 'COM'), P(rly, 'NO'); osc, tcom = P(top, '4  O/S/C'), P(top, '5  COM')
    line(d, com, (12.6, com.y), (12.6, osc.y), osc, color=SIG)
    line(d, no, (12.1, no.y), (12.1, tcom.y), tcom, color=SIG)
    # opto outputs to ESP
    g27, g14 = P(esp, 'GPIO27'), P(esp, 'GPIO14')
    line(d, g27, (7.0, g27.y), (7.0, o1.collector.y), o1.collector, color=SIG)
    line(d, g14, (6.5, g14.y), (6.5, o2.collector.y), o2.collector, color=SIG)
    d.add(elm.Label().at((7.15, o1.collector.y + 0.6)).label('opening pulses', fontsize=8.5, halign='left', color=SIG))
    d.add(elm.Label().at((6.65, o2.collector.y + 0.6)).label('closing pulses', fontsize=8.5, halign='left', color=SIG))
    # LED side: one series resistor from MOTOR1 (15), LEDs anti-parallel, return to MOTOR1 (16)
    m15, m16 = P(top, '15  MOTOR1'), P(top, '16  MOTOR1')
    nodeA = (12.0, o1.anode.y)         # shared node after R
    r = d.add(elm.Resistor().at(m15).left().length(2.0).label('R1  2.7 kΩ ½ W', fontsize=8.5, ofst=0.25))
    line(d, r.end, (nodeA[0], m15.y), nodeA)
    dot(d, nodeA)
    line(d, nodeA, o1.anode, color=SIG)
    line(d, nodeA, (nodeA[0], o2.cathode.y), o2.cathode, color=SIG)
    dot(d, (nodeA[0], o2.cathode.y))
    nodeB = (11.4, o1.cathode.y)
    line(d, m16, (12.9, m16.y), (12.9, o2.cathode.y - 0.6), (nodeB[0], o2.cathode.y - 0.6), nodeB)
    dot(d, nodeB)
    line(d, nodeB, o1.cathode, color=SIG)
    line(d, nodeB, (nodeB[0], o2.anode.y), o2.anode, color=SIG)
    dot(d, (nodeB[0], o2.anode.y))
    note(d, (0, -4.0),
         'MOTOR1 is 24 V DC and the board reverses it to reverse the gate. R1 feeds both LEDs, wired\n'
         'anti-parallel, so motor polarity selects U1 (opening) or U2 (closing) and the conducting LED\n'
         'clamps the other to about 1.2 V. The drive is chopped, so the ESP sees each run as a pulse\n'
         'burst; GPIO14 and GPIO27 use internal pull-ups and count pulses per minute.\n'
         'Relay contacts across O/S/C and COM (terminals 4 and 5) do what the key fob does.')
    d.save(f'{OUT}/gate-schematic.svg')
    print('gate ok')


def v33(d, at):
    d.add(elm.Vdd().at(at).color(PWR).label('+3V3', fontsize=9, color=PWR))

def v5(d, at):
    d.add(elm.Vdd().at(at).color(PWR).label('+5V', fontsize=9, color=PWR))

def gnd(d, at):
    d.add(elm.Ground().at(at))

def jog(d, a, b, x, color=SIG):
    """Orthogonal wire a -> b with the vertical segment at x."""
    line(d, a, (x, a.y), (x, b.y), b, color=color)

def tag_out(d, at, text, w=1.5):
    """Net label leaving a pin to the right."""
    t = d.add(elm.Tag(width=w).at(at).right().label(text, fontsize=8.5, color=SIG).color(SIG))
    return t

def tag_in(d, at, text, w=1.5):
    """Net label arriving at a pin from the left."""
    t = d.add(elm.Tag(width=w).at(at).left().label(text, fontsize=8.5, color=SIG).color(SIG))
    return t

def sauna():
    d = new()
    esp = d.add(box('ESP32 DevKit', (3.8, 8.8), left=['VIN', '3V3', 'GND'],
                    right=['GPIO15', 'GPIO21', 'GPIO22', 'GPIO25', 'GPIO26', 'GPIO27', 'GPIO12', 'GPIO13', 'GPIO14'], spacing=0.85).at((0, 0)))
    v5(d, P(esp, 'VIN')); gnd(d, P(esp, 'inL1'))
    t = P(esp, '3V3'); line(d, t, (t.x - 1.0, t.y)); v33(d, (t.x - 1.0, t.y))
    nets = {'GPIO15': '1-WIRE', 'GPIO21': 'SDA', 'GPIO22': 'SCL', 'GPIO25': 'ENC_A', 'GPIO26': 'ENC_B', 'GPIO27': 'ENC_SW', 'GPIO12': 'RLY1', 'GPIO13': 'RLY2', 'GPIO14': 'RLY3'}
    for pin, net in nets.items():
        tag_out(d, P(esp, pin), net)
    # Peripherals, each wired by net name. Supply pin on top, ground at the bottom.
    X = 10.0
    ds = d.add(box('DS18B20 probe', (3.0, 2.4), left=['VDD', 'DQ', 'GND'], spacing=0.7).at((X, 7.6)))
    oled = d.add(box('SSD1306 OLED, I²C', (3.0, 2.9), left=['VCC', 'SDA', 'SCL', 'GND'], spacing=0.7).at((X, 3.6)))
    enc = d.add(box('KY-040 rotary encoder', (3.0, 3.6), left=['+', 'CLK', 'DT', 'SW', 'GND'], spacing=0.7).at((X, -1.2)))
    # Three single relay modules. Each one only breaks the hot line to its pair of bulbs.
    relays = []
    for i in range(3):
        y = -5.2 - i * 3.0
        r = d.add(box(f'Relay {i+1}, 5 V coil, 10 A', (3.4, 2.2), left=['VCC', 'IN', 'GND'], right=['COM', 'NO'], spacing=0.7).at((X, y)))
        v5(d, P(r, 'VCC')); gnd(d, P(r, 'GND')); tag_in(d, P(r, 'IN'), f'RLY{i+1}')
        relays.append(r)
    v33(d, P(ds, 'VDD')); gnd(d, P(ds, 'GND'))
    v33(d, P(oled, 'VCC')); gnd(d, P(oled, 'GND'))
    v33(d, P(enc, '+')); gnd(d, P(enc, 'GND'))
    # 1-Wire: net label, then the local pull-up to the probe's VDD net
    dq, vdd = P(ds, 'DQ'), P(ds, 'VDD')
    node = (dq.x - 1.0, dq.y)
    line(d, dq, node, color=SIG); dot(d, node)
    line(d, node, (dq.x - 2.6, dq.y), color=SIG)
    tag_in(d, (dq.x - 2.6, dq.y), '1-WIRE')
    r1 = d.add(elm.Resistor().endpoints(node, (node[0], vdd.y)).label('R1  4.7 kΩ', fontsize=8.5, loc='left', ofst=0.35))
    line(d, (node[0], vdd.y), vdd); dot(d, (vdd.x, vdd.y))
    tag_in(d, P(oled, 'SDA'), 'SDA'); tag_in(d, P(oled, 'SCL'), 'SCL')
    tag_in(d, P(enc, 'CLK'), 'ENC_A'); tag_in(d, P(enc, 'DT'), 'ENC_B'); tag_in(d, P(enc, 'SW'), 'ENC_SW')
    # Mains: one hot line feeds every COM; each NO goes through its bulb pair to neutral.
    coms = [P(r, 'COM') for r in relays]; nos = [P(r, 'NO') for r in relays]
    lx = coms[0].x + 0.9; nx = coms[0].x + 6.2
    line(d, (lx, coms[0].y + 1.2), (lx, coms[-1].y), color=MAINS)
    d.add(elm.Label().at((lx, coms[0].y + 1.35)).label('L  120 V AC in', fontsize=9, color=MAINS, halign='center', valign='bottom'))
    for c in coms:
        line(d, c, (lx, c.y), color=MAINS); dot(d, (lx, c.y))
    line(d, (nx, coms[0].y + 1.2), (nx, nos[-1].y), color=MAINS)
    d.add(elm.Label().at((nx, coms[0].y + 1.35)).label('N', fontsize=9, color=MAINS, halign='center', valign='bottom'))
    for i, n in enumerate(nos):
        line(d, n, (lx + 0.6, n.y), color=MAINS)
        lamp = d.add(elm.Lamp().at((lx + 0.6, n.y)).right().length(1.8).scale(0.75).color(MAINS))
        line(d, lamp.end, (nx, n.y), color=MAINS); dot(d, (nx, n.y))
        d.add(elm.Label().at((lamp.end.x + 0.25, n.y + 0.1)).label(f'bulbs {2*i+1} + {2*i+2},  2 × 250 W', fontsize=8, halign='left', valign='bottom', color=MAINS))
    d.add(elm.Label().at((X, coms[-1].y - 2.0)).label(
        'Each relay interrupts only the hot line to its pair of bulbs; neutral runs straight to the bulbs.\n'
        'Mains side: 14 AWG, grounded metal enclosure, breaker sized for the 12.6 A total.\n'
        'Keep it physically apart from the 3.3 V wiring.', fontsize=8.5, halign='left', valign='top', color=MAINS))
    note(d, (0, -3.0),
         'Signals are shown as net labels: a tag on an ESP pin connects\n'
         'to the tag of the same name on the peripheral.\n\n'
         'Staged thermostat: 1, 2, or 3 relays on depending on how far\n'
         'below the setpoint the room is. 2 °F deadband, 15 s minimum\n'
         'on/off per bank, wear leveling by cycle count and runtime.\n'
         'The encoder button uses the internal pull-up. Firmware in\n'
         'PlatformIO and ESPHome forms.')
    d.save(f'{OUT}/sauna-schematic.svg'); print('sauna ok')

def salt():
    d = new()
    esp = d.add(box('ESP32 DevKit  (ESPHome)', (3.6, 4.0), left=['VIN', 'GND'], right=['GPIO23', 'GPIO22'], spacing=1.0).at((0, 0)))
    v5(d, P(esp, 'VIN')); gnd(d, P(esp, 'GND'))
    hc = d.add(box('HC-SR04 ultrasonic', (3.2, 3.6), left=['VCC', 'TRIG', 'ECHO', 'GND'], spacing=0.8).at((9.0, 0.2)))
    v5(d, P(hc, 'VCC')); gnd(d, P(hc, 'GND'))
    jog(d, P(esp, 'GPIO23'), P(hc, 'TRIG'), 6.0)
    d.add(elm.Label().at((6.1, P(hc, 'TRIG').y + 0.25)).label('TRIG', fontsize=8.5, halign='left', color=SIG))
    jog(d, P(esp, 'GPIO22'), P(hc, 'ECHO'), 5.4)
    d.add(elm.Label().at((5.5, P(hc, 'ECHO').y + 0.25)).label('ECHO, wired direct', fontsize=8.5, halign='left', color=SIG))
    note(d, (0, -2.4),
         'ECHO is a 5 V output driven straight into GPIO22, which is out of spec for the ESP32 but has run\n'
         'since February 2026 without trouble. A 1 kΩ / 2 kΩ divider on ECHO is the by-the-book version.\n'
         'Sensor faces straight down from the brine tank lid, above the highest brine level. In the YAML,\n'
         '30 cm = full and 50 cm = empty; measure your own tank and change both numbers.')
    d.save(f'{OUT}/salt-schematic.svg'); print('salt ok')

def imac():
    d = new()
    pc = d.add(box('Computer', (2.8, 2.0), right=['DP'], spacing=1).at((0, 1.6)))
    psu = d.add(box('Power adapter (kit)', (2.8, 1.6), right=['DC'], spacing=1).at((0, -1.6)))
    brd = d.add(box('5K eDP controller board (LM270QQ1)', (4.6, 3.6), left=['DP IN', 'HDMI', 'PWR'], right=['eDP 1', 'eDP 2', 'BL'], spacing=0.9).at((6.0, 0.2)))
    pnl = d.add(box('LM270QQ1 panel, 5120 × 2880', (3.8, 3.2), left=['eDP 1', 'eDP 2'], bottom=['LED'], spacing=0.9).at((15.0, 1.2)))
    bl = d.add(box('Backlight driver (kit)', (3.2, 1.6), left=['IN'], right=['LED'], spacing=1).at((12.0, -3.4)))
    jog(d, P(pc, 'DP'), P(brd, 'DP IN'), 4.6)
    d.add(elm.Label().at((4.7, P(brd, 'DP IN').y + 0.25)).label('DisplayPort 1.4, 5K at 60 Hz', fontsize=8.5, halign='left', color=SIG))
    jog(d, P(psu, 'DC'), P(brd, 'PWR'), 4.0, color=PWR)
    jog(d, P(brd, 'eDP 1'), P(pnl, 'eDP 1'), 12.8)
    jog(d, P(brd, 'eDP 2'), P(pnl, 'eDP 2'), 12.4)
    jog(d, P(brd, 'BL'), P(bl, 'IN'), 11.4, color=PWR)
    led_out = P(bl, 'LED'); led_in = P(pnl, 'LED')
    line(d, led_out, (led_in.x, led_out.y), led_in, color=PWR)
    note(d, (0, -4.6), 'No keypad and no on-screen menu: the board passes DisplayPort straight to the panel\'s eDP inputs.\nHDMI works only below 5K 60 Hz. The board and driver hang out of the bottom vent slot on their cables.')
    d.save(f'{OUT}/imac-schematic.svg'); print('imac ok')

if __name__ == '__main__':
    gate(); sauna(); salt(); imac()
