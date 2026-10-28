import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.widgets import CheckButtons, TextBox, Button

# ------------------------------------------------------------
# Daten einlesen
# ------------------------------------------------------------
data = pd.read_csv("flowlog_054.csv")
data["time_min"] = data["millis_ms"] / (1000 * 60)   # ms -> Minuten

channels = [f"ch{i}" for i in range(1, 8)]
flow_cols = [f"{ch}_flow_mean_ulmin" for ch in channels]
p_cols = [f"{ch}_p_mean_mbar" for ch in channels]

# ------------------------------------------------------------
# Figur mit zwei Subplots (links) + Bedienelemente (rechts)
# ------------------------------------------------------------
fig, (ax_flow, ax_p) = plt.subplots(2, 1, sharex=True, figsize=(15, 9))
fig.subplots_adjust(left=0.06, right=0.78, top=0.95, bottom=0.07, hspace=0.12)

colors = plt.rcParams["axes.prop_cycle"].by_key()["color"]

flow_lines = []
for col, ch, c in zip(flow_cols, channels, colors):
    (line,) = ax_flow.plot(data["time_min"], data[col], label=ch, color=c, lw=0.8)
    flow_lines.append(line)

p_lines = []
for col, ch, c in zip(p_cols, channels, colors):
    (line,) = ax_p.plot(data["time_min"], data[col], label=ch, color=c, lw=0.8)
    p_lines.append(line)

# ------------------------------------------------------------
# Vertikale Linien für _air > 0
#   Pro Kanal wird an jedem Zeitpunkt, an dem _air von 0 auf > 0
#   wechselt, eine gestrichelte Linie in der Kanalfarbe gezeichnet.
#   (Eine Linie pro Messpunkt wäre bei Dauer-"air" > 290.000 Linien.)
# ------------------------------------------------------------
def air_onsets(ch):
    air = data[f"{ch}_air"] > 0
    start = air & ~air.shift(fill_value=False)
    return data.loc[start, "time_min"].to_numpy()


air_times = {ch: air_onsets(ch) for ch in channels}
for ch in channels:
    print(f"{ch}: {len(air_times[ch])}x air > 0 (Beginn bei",
          ", ".join(f"{t:.1f}" for t in air_times[ch]) or "-", "min)")


def draw_air_lines(ax):
    """Gibt pro Kanal eine Liste der vertikalen Linien zurück."""
    result = []
    for ch, c in zip(channels, colors):
        result.append([ax.axvline(t, color=c, ls="--", lw=1.2, alpha=0.8)
                       for t in air_times[ch]])
    return result


flow_air = draw_air_lines(ax_flow)
p_air = draw_air_lines(ax_p)

# Legendeneintrag für die Air-Linien
from matplotlib.lines import Line2D
air_handle = Line2D([], [], color="gray", ls="--", lw=1.2, label="air > 0")

ax_flow.set_title("Flow (Mittelwert)")
ax_flow.set_ylabel("Flow [µl/min]")
ax_flow.grid(True)
ax_flow.legend(handles=flow_lines + [air_handle], loc="upper left", ncol=8, fontsize=8)

ax_p.set_title("Druck (Mittelwert)")
ax_p.set_ylabel("Druck [mbar]")
ax_p.set_xlabel("Zeit [min]")
ax_p.grid(True)
ax_p.legend(handles=p_lines + [air_handle], loc="upper left", ncol=8, fontsize=8)


# ------------------------------------------------------------
# Hilfsfunktion: y-Achse automatisch an sichtbare Linien anpassen
# ------------------------------------------------------------
def autoscale_visible(ax, lines):
    xmin, xmax = ax.get_xlim()
    ymins, ymaxs = [], []
    for ln in lines:
        if not ln.get_visible():
            continue
        x, y = ln.get_xdata(), ln.get_ydata()
        mask = (x >= xmin) & (x <= xmax)
        if mask.any():
            ymins.append(np.nanmin(y[mask]))
            ymaxs.append(np.nanmax(y[mask]))
    if ymins:
        lo, hi = min(ymins), max(ymaxs)
        pad = 0.05 * (hi - lo) if hi > lo else 1.0
        ax.set_ylim(lo - pad, hi + pad)


# ------------------------------------------------------------
# Bedienelemente für einen Subplot erzeugen
#   - CheckButtons: Datenreihen ein-/ausblenden
#   - TextBox Ymin / Ymax: y-Achsen-Grenzen setzen (Enter drücken)
#   - Button "Auto": y-Achse an sichtbare Daten anpassen
# ------------------------------------------------------------
def make_controls(ax, lines, air_lines, top):
    x0, w = 0.81, 0.17

    # Checkboxen
    ax_chk = fig.add_axes([x0, top - 0.25, w, 0.25])
    ax_chk.set_title("Kanäle", fontsize=9)
    names = [ln.get_label() for ln in lines]
    air_label = "air-Linien"
    chk = CheckButtons(ax_chk, names + [air_label], [True] * (len(lines) + 1))
    for lbl, ln in zip(chk.labels, lines):
        lbl.set_color(ln.get_color())

    def update_air():
        # Air-Linie sichtbar, wenn Kanal UND "air-Linien" angehakt sind
        show_air = chk.get_status()[-1]
        for ln, vlines in zip(lines, air_lines):
            for v in vlines:
                v.set_visible(show_air and ln.get_visible())

    def on_check(label):
        if label in names:
            idx = names.index(label)
            lines[idx].set_visible(not lines[idx].get_visible())
        update_air()
        fig.canvas.draw_idle()

    chk.on_clicked(on_check)

    # Textfelder für Ymin / Ymax
    ymin0, ymax0 = ax.get_ylim()
    ax_min = fig.add_axes([x0 + 0.04, top - 0.31, w - 0.04, 0.035])
    ax_max = fig.add_axes([x0 + 0.04, top - 0.355, w - 0.04, 0.035])
    tb_min = TextBox(ax_min, "Ymin ", initial=f"{ymin0:.3g}")
    tb_max = TextBox(ax_max, "Ymax ", initial=f"{ymax0:.3g}")

    def set_ylim(_text=None):
        try:
            lo = float(tb_min.text.replace(",", "."))
            hi = float(tb_max.text.replace(",", "."))
        except ValueError:
            return
        if lo < hi:
            ax.set_ylim(lo, hi)
            fig.canvas.draw_idle()

    tb_min.on_submit(set_ylim)
    tb_max.on_submit(set_ylim)

    # Auto-Button
    ax_btn = fig.add_axes([x0 + 0.04, top - 0.40, w - 0.04, 0.035])
    btn = Button(ax_btn, "Auto (sichtbare Reihen)")

    def on_auto(_event):
        autoscale_visible(ax, lines)
        lo, hi = ax.get_ylim()
        tb_min.set_val(f"{lo:.3g}")
        tb_max.set_val(f"{hi:.3g}")
        fig.canvas.draw_idle()

    btn.on_clicked(on_auto)

    # Textfelder aktualisieren, wenn per Zoom/Pan die y-Achse geändert wird
    def on_ylim_changed(axis):
        lo, hi = axis.get_ylim()
        # eventson=False verhindert Rückkopplung über on_submit
        tb_min.eventson = tb_max.eventson = False
        tb_min.set_val(f"{lo:.3g}")
        tb_max.set_val(f"{hi:.3g}")
        tb_min.eventson = tb_max.eventson = True

    ax.callbacks.connect("ylim_changed", on_ylim_changed)

    # Referenzen zurückgeben, damit die Widgets nicht vom Garbage Collector entfernt werden
    return chk, tb_min, tb_max, btn


widgets_flow = make_controls(ax_flow, flow_lines, flow_air, top=0.95)
widgets_p = make_controls(ax_p, p_lines, p_air, top=0.50)


# ------------------------------------------------------------
# Bedienelemente für die x-Achse (Zeit)
#   Beide Subplots teilen sich die x-Achse (sharex=True),
#   daher gibt es nur ein Xmin/Xmax-Paar für beide.
# ------------------------------------------------------------
t_min_all = data["time_min"].min()
t_max_all = data["time_min"].max()
xmin0, xmax0 = ax_p.get_xlim()

fig.text(0.81, 0.075, "Zeitachse [min]", fontsize=9)
ax_xmin = fig.add_axes([0.85, 0.035, 0.05, 0.03])
ax_xmax = fig.add_axes([0.93, 0.035, 0.05, 0.03])
tb_xmin = TextBox(ax_xmin, "Xmin ", initial=f"{xmin0:.1f}")
tb_xmax = TextBox(ax_xmax, "Xmax ", initial=f"{xmax0:.1f}")


def set_xlim(_text=None):
    try:
        lo = float(tb_xmin.text.replace(",", "."))
        hi = float(tb_xmax.text.replace(",", "."))
    except ValueError:
        return
    if lo < hi:
        ax_p.set_xlim(lo, hi)          # wirkt durch sharex auf beide Subplots
        fig.canvas.draw_idle()


tb_xmin.on_submit(set_xlim)
tb_xmax.on_submit(set_xlim)

ax_xbtn = fig.add_axes([0.85, 0.0, 0.13, 0.03])
btn_x = Button(ax_xbtn, "Auto X (gesamt)")


def on_auto_x(_event):
    ax_p.set_xlim(t_min_all, t_max_all)
    fig.canvas.draw_idle()


btn_x.on_clicked(on_auto_x)


# Textfelder aktualisieren, wenn per Zoom/Pan die x-Achse geändert wird
def on_xlim_changed(axis):
    lo, hi = axis.get_xlim()
    tb_xmin.eventson = tb_xmax.eventson = False
    tb_xmin.set_val(f"{lo:.1f}")
    tb_xmax.set_val(f"{hi:.1f}")
    tb_xmin.eventson = tb_xmax.eventson = True


ax_p.callbacks.connect("xlim_changed", on_xlim_changed)
widgets_x = (tb_xmin, tb_xmax, btn_x)

plt.show()
print(data)