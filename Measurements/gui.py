"""
Flowlog-Viewer (PyQt5 + Matplotlib)

Funktionen:
  - CSV-Datei laden (Button "CSV laden…" oder als Kommandozeilen-Argument)
  - Oberer Subplot: ch1..ch7 _flow_mean_ulmin
  - Unterer Subplot: ch1..ch7 _p_mean_mbar
  - Grid in beiden Subplots
  - Kanäle pro Subplot ein-/ausblenden
  - Vertikale, gestrichelte Linien am Beginn jeder Phase mit _air > 0
    (ein-/ausblendbar, folgen der Sichtbarkeit des Kanals)
  - Ymin/Ymax pro Subplot, "Auto Y" für sichtbare Reihen
  - Xmin/Xmax (gemeinsame Zeitachse), "Auto X" für Gesamtzeitraum
  - Matplotlib-Werkzeugleiste (Zoom, Verschieben, Speichern);
    Eingabefelder werden beim Zoomen automatisch aktualisiert

Start:   python flowlog_gui.py               (leeres Fenster, dann CSV laden)
         python flowlog_gui.py flowlog_054.csv
"""

import os
import sys

import numpy as np
import pandas as pd

from PyQt5 import QtCore, QtWidgets
from matplotlib.backends.backend_qt5agg import (
    FigureCanvasQTAgg as FigureCanvas,
    NavigationToolbar2QT as NavigationToolbar,
)
from matplotlib.figure import Figure
from matplotlib.lines import Line2D

CHANNELS = [f"ch{i}" for i in range(1, 8)]
COLORS = ["#1f77b4", "#ff7f0e", "#2ca02c", "#d62728",
          "#9467bd", "#8c564b", "#e377c2"]

# Konfiguration der beiden Subplots
PLOTS = [
    {"key": "flow", "suffix": "_flow_mean_ulmin",
     "title": "Flow (Mittelwert)", "ylabel": "Flow [µl/min]"},
    {"key": "p", "suffix": "_p_mean_mbar",
     "title": "Druck (Mittelwert)", "ylabel": "Druck [mbar]"},
]


def parse_float(text):
    """Text -> float, akzeptiert Komma als Dezimaltrennzeichen."""
    try:
        return float(text.strip().replace(",", "."))
    except ValueError:
        return None


# ======================================================================
# Bedienfeld für einen Subplot (Kanäle, air-Linien, Ymin/Ymax, Auto)
# ======================================================================
class PlotControls(QtWidgets.QGroupBox):
    visibilityChanged = QtCore.pyqtSignal()
    ylimRequested = QtCore.pyqtSignal(float, float)
    autoRequested = QtCore.pyqtSignal()

    def __init__(self, title, parent=None):
        super().__init__(title, parent)
        layout = QtWidgets.QVBoxLayout(self)

        # Kanal-Checkboxen in zwei Spalten
        grid = QtWidgets.QGridLayout()
        self.channel_boxes = []
        for i, (ch, c) in enumerate(zip(CHANNELS, COLORS)):
            cb = QtWidgets.QCheckBox(ch)
            cb.setChecked(True)
            cb.setStyleSheet(f"QCheckBox {{ color: {c}; font-weight: bold; }}")
            cb.toggled.connect(self.visibilityChanged)
            grid.addWidget(cb, i % 4, i // 4)
            self.channel_boxes.append(cb)

        self.air_box = QtWidgets.QCheckBox("air-Linien")
        self.air_box.setChecked(True)
        self.air_box.toggled.connect(self.visibilityChanged)
        grid.addWidget(self.air_box, 3, 1)
        layout.addLayout(grid)

        # Alle an / alle aus
        row = QtWidgets.QHBoxLayout()
        btn_all = QtWidgets.QPushButton("Alle")
        btn_none = QtWidgets.QPushButton("Keine")
        btn_all.clicked.connect(lambda: self._set_all(True))
        btn_none.clicked.connect(lambda: self._set_all(False))
        row.addWidget(btn_all)
        row.addWidget(btn_none)
        layout.addLayout(row)

        # Ymin / Ymax
        form = QtWidgets.QFormLayout()
        self.ymin_edit = QtWidgets.QLineEdit()
        self.ymax_edit = QtWidgets.QLineEdit()
        for e in (self.ymin_edit, self.ymax_edit):
            e.returnPressed.connect(self._emit_ylim)
        form.addRow("Ymin:", self.ymin_edit)
        form.addRow("Ymax:", self.ymax_edit)
        layout.addLayout(form)

        row2 = QtWidgets.QHBoxLayout()
        btn_apply = QtWidgets.QPushButton("Übernehmen")
        btn_apply.clicked.connect(self._emit_ylim)
        btn_auto = QtWidgets.QPushButton("Auto Y")
        btn_auto.setToolTip("y-Achse an sichtbare Reihen im aktuellen Zeitbereich anpassen")
        btn_auto.clicked.connect(self.autoRequested)
        row2.addWidget(btn_apply)
        row2.addWidget(btn_auto)
        layout.addLayout(row2)

    def _set_all(self, state):
        for cb in self.channel_boxes:
            cb.blockSignals(True)
            cb.setChecked(state)
            cb.blockSignals(False)
        self.visibilityChanged.emit()

    def _emit_ylim(self):
        lo, hi = parse_float(self.ymin_edit.text()), parse_float(self.ymax_edit.text())
        if lo is not None and hi is not None and lo < hi:
            self.ylimRequested.emit(lo, hi)

    def channel_visible(self, i):
        return self.channel_boxes[i].isChecked()

    def air_visible(self):
        return self.air_box.isChecked()

    def show_ylim(self, lo, hi):
        self.ymin_edit.setText(f"{lo:.4g}")
        self.ymax_edit.setText(f"{hi:.4g}")


# ======================================================================
# Hauptfenster
# ======================================================================
class MainWindow(QtWidgets.QMainWindow):
    def __init__(self, csv_path=None):
        super().__init__()
        self.setWindowTitle("Flowlog-Viewer")
        self.resize(1500, 900)

        self.data = None
        self.lines = {}      # key -> [Line2D je Kanal]
        self.air_lines = {}  # key -> [[axvline...] je Kanal]

        # ---------- Matplotlib-Figur ----------
        self.fig = Figure(figsize=(12, 8))
        self.canvas = FigureCanvas(self.fig)
        self.toolbar = NavigationToolbar(self.canvas, self)
        ax_flow = self.fig.add_subplot(211)
        ax_p = self.fig.add_subplot(212, sharex=ax_flow)
        self.axes = {"flow": ax_flow, "p": ax_p}
        self.fig.subplots_adjust(left=0.07, right=0.98, top=0.95,
                                 bottom=0.07, hspace=0.15)

        plot_widget = QtWidgets.QWidget()
        pl = QtWidgets.QVBoxLayout(plot_widget)
        pl.setContentsMargins(0, 0, 0, 0)
        pl.addWidget(self.toolbar)
        pl.addWidget(self.canvas)

        # ---------- Seitenleiste ----------
        side = QtWidgets.QWidget()
        side.setFixedWidth(300)
        sl = QtWidgets.QVBoxLayout(side)

        self.btn_load = QtWidgets.QPushButton("CSV laden …")
        self.btn_load.setMinimumHeight(36)
        self.btn_load.clicked.connect(self.open_csv_dialog)
        sl.addWidget(self.btn_load)
        self.file_label = QtWidgets.QLabel("Keine Datei geladen")
        self.file_label.setWordWrap(True)
        sl.addWidget(self.file_label)

        self.controls = {}
        for cfg in PLOTS:
            ctrl = PlotControls(cfg["title"])
            key = cfg["key"]
            ctrl.visibilityChanged.connect(lambda k=key: self.update_visibility(k))
            ctrl.ylimRequested.connect(lambda lo, hi, k=key: self.set_ylim(k, lo, hi))
            ctrl.autoRequested.connect(lambda k=key: self.auto_y(k))
            self.controls[key] = ctrl
            sl.addWidget(ctrl)

        # x-Achse
        xbox = QtWidgets.QGroupBox("Zeitachse [min] (beide Plots)")
        xl = QtWidgets.QVBoxLayout(xbox)
        xform = QtWidgets.QFormLayout()
        self.xmin_edit = QtWidgets.QLineEdit()
        self.xmax_edit = QtWidgets.QLineEdit()
        for e in (self.xmin_edit, self.xmax_edit):
            e.returnPressed.connect(self.apply_xlim)
        xform.addRow("Xmin:", self.xmin_edit)
        xform.addRow("Xmax:", self.xmax_edit)
        xl.addLayout(xform)
        xrow = QtWidgets.QHBoxLayout()
        bx_apply = QtWidgets.QPushButton("Übernehmen")
        bx_apply.clicked.connect(self.apply_xlim)
        bx_auto = QtWidgets.QPushButton("Auto X (gesamt)")
        bx_auto.clicked.connect(self.auto_x)
        xrow.addWidget(bx_apply)
        xrow.addWidget(bx_auto)
        xl.addLayout(xrow)
        sl.addWidget(xbox)

        # air-Übersicht
        airbox = QtWidgets.QGroupBox("Beginn air > 0 [min]")
        al = QtWidgets.QVBoxLayout(airbox)
        self.air_text = QtWidgets.QPlainTextEdit()
        self.air_text.setReadOnly(True)
        al.addWidget(self.air_text)
        sl.addWidget(airbox, stretch=1)

        # ---------- Gesamtlayout ----------
        central = QtWidgets.QWidget()
        hl = QtWidgets.QHBoxLayout(central)
        hl.addWidget(plot_widget, stretch=1)
        hl.addWidget(side)
        self.setCentralWidget(central)

        self.draw_empty()
        if csv_path:
            self.load_csv(csv_path)

    # ------------------------------------------------------------------
    # Daten laden
    # ------------------------------------------------------------------
    def open_csv_dialog(self):
        start_dir = os.path.dirname(self.file_label.toolTip()) or os.getcwd()
        path, _ = QtWidgets.QFileDialog.getOpenFileName(
            self, "CSV-Datei öffnen", start_dir, "CSV-Dateien (*.csv);;Alle Dateien (*)")
        if path:
            self.load_csv(path)

    def load_csv(self, path):
        QtWidgets.QApplication.setOverrideCursor(QtCore.Qt.WaitCursor)
        try:
            data = pd.read_csv(path)
            needed = ["millis_ms"] + [f"{ch}{p['suffix']}" for ch in CHANNELS for p in PLOTS] \
                     + [f"{ch}_air" for ch in CHANNELS]
            missing = [c for c in needed if c not in data.columns]
            if missing:
                raise ValueError("Fehlende Spalten: " + ", ".join(missing[:10]))
        except Exception as exc:
            QtWidgets.QApplication.restoreOverrideCursor()
            QtWidgets.QMessageBox.critical(self, "Fehler beim Laden",
                                           f"{os.path.basename(path)}\n\n{exc}")
            return

        data["time_min"] = data["millis_ms"] / (1000 * 60)   # ms -> Minuten
        self.data = data
        self.file_label.setText(f"<b>{os.path.basename(path)}</b><br>{len(data):,} Zeilen"
                                .replace(",", "."))
        self.file_label.setToolTip(path)
        self.setWindowTitle(f"Flowlog-Viewer – {os.path.basename(path)}")
        self.plot_data()
        QtWidgets.QApplication.restoreOverrideCursor()

    # ------------------------------------------------------------------
    # Zeichnen
    # ------------------------------------------------------------------
    def setup_axes(self):
        for cfg in PLOTS:
            ax = self.axes[cfg["key"]]
            ax.set_title(cfg["title"])
            ax.set_ylabel(cfg["ylabel"])
            ax.grid(True)
        self.axes["p"].set_xlabel("Zeit [min]")
        self.axes["flow"].tick_params(labelbottom=False)

        # Eingabefelder bei Zoom/Pan nachführen.
        # (ax.clear() löscht die Callbacks, daher hier jedes Mal neu verbinden)
        for key, ax in self.axes.items():
            ax.callbacks.connect("ylim_changed",
                                 lambda a, k=key: self.controls[k].show_ylim(*a.get_ylim()))
        self.axes["p"].callbacks.connect("xlim_changed",
                                         lambda a: self.show_xlim(*a.get_xlim()))

    def draw_empty(self):
        for ax in self.axes.values():
            ax.clear()
        self.setup_axes()
        self.canvas.draw_idle()

    def air_onsets(self, ch):
        air = self.data[f"{ch}_air"] > 0
        start = air & ~air.shift(fill_value=False)
        return self.data.loc[start, "time_min"].to_numpy()

    def plot_data(self):
        t = self.data["time_min"].to_numpy()
        air_times = {ch: self.air_onsets(ch) for ch in CHANNELS}
        air_handle = Line2D([], [], color="gray", ls="--", lw=1.2, label="air > 0")

        for cfg in PLOTS:
            key, ax = cfg["key"], self.axes[cfg["key"]]
            ax.clear()
            self.lines[key] = []
            self.air_lines[key] = []
            for ch, c in zip(CHANNELS, COLORS):
                (ln,) = ax.plot(t, self.data[f"{ch}{cfg['suffix']}"].to_numpy(),
                                label=ch, color=c, lw=0.8)
                self.lines[key].append(ln)
                self.air_lines[key].append(
                    [ax.axvline(x, color=c, ls="--", lw=1.2, alpha=0.8)
                     for x in air_times[ch]])
            ax.legend(handles=self.lines[key] + [air_handle],
                      loc="upper left", ncol=8, fontsize=8)
        self.setup_axes()

        # Übersicht der air-Zeitpunkte
        txt = []
        for ch in CHANNELS:
            ts = air_times[ch]
            txt.append(f"{ch}: " + (", ".join(f"{x:.1f}" for x in ts) if len(ts) else "–"))
        self.air_text.setPlainText("\n".join(txt))

        for key in self.axes:
            self.update_visibility(key, redraw=False)
        self.auto_x(redraw=False)
        for key in self.axes:
            self.auto_y(key, redraw=False)
        self.canvas.draw_idle()

    # ------------------------------------------------------------------
    # Interaktion
    # ------------------------------------------------------------------
    def update_visibility(self, key, redraw=True):
        if key not in self.lines:
            return
        ctrl = self.controls[key]
        for i, (ln, vlines) in enumerate(zip(self.lines[key], self.air_lines[key])):
            vis = ctrl.channel_visible(i)
            ln.set_visible(vis)
            for v in vlines:
                v.set_visible(vis and ctrl.air_visible())
        if redraw:
            self.canvas.draw_idle()

    def set_ylim(self, key, lo, hi):
        self.axes[key].set_ylim(lo, hi)
        self.canvas.draw_idle()

    def auto_y(self, key, redraw=True):
        """y-Achse an sichtbare Linien im aktuellen x-Bereich anpassen."""
        if key not in self.lines:
            return
        ax = self.axes[key]
        xmin, xmax = ax.get_xlim()
        ymins, ymaxs = [], []
        for ln in self.lines[key]:
            if not ln.get_visible():
                continue
            x, y = ln.get_xdata(), ln.get_ydata()
            mask = (x >= xmin) & (x <= xmax) & np.isfinite(y)
            if mask.any():
                ymins.append(y[mask].min())
                ymaxs.append(y[mask].max())
        if ymins:
            lo, hi = min(ymins), max(ymaxs)
            pad = 0.05 * (hi - lo) if hi > lo else 1.0
            ax.set_ylim(lo - pad, hi + pad)
        if redraw:
            self.canvas.draw_idle()

    def apply_xlim(self):
        lo, hi = parse_float(self.xmin_edit.text()), parse_float(self.xmax_edit.text())
        if lo is not None and hi is not None and lo < hi:
            self.axes["p"].set_xlim(lo, hi)   # sharex -> beide Plots
            self.canvas.draw_idle()

    def auto_x(self, redraw=True):
        if self.data is None:
            return
        t = self.data["time_min"]
        self.axes["p"].set_xlim(t.min(), t.max())
        if redraw:
            self.canvas.draw_idle()

    def show_xlim(self, lo, hi):
        self.xmin_edit.setText(f"{lo:.1f}")
        self.xmax_edit.setText(f"{hi:.1f}")


def main():
    app = QtWidgets.QApplication(sys.argv)
    csv_path = sys.argv[1] if len(sys.argv) > 1 else None
    win = MainWindow(csv_path)
    win.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()