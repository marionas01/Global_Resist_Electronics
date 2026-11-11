"""
Flowlog-Viewer (PyQt5 + pyqtgraph, reine Qt-Oberfläche)

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
  - Umschalter Zeiteinheit Minuten <-> Stunden (Ausschnitt bleibt erhalten)
  - Werkzeugleiste (QToolBar): Gesamtansicht, Zoom-Rechteck/Verschieben,
    als Bild speichern; Mausrad zoomt, Ziehen verschiebt.
    Eingabefelder werden beim Zoomen automatisch aktualisiert.
  - Cursor-Koordinaten in der Statusleiste

Benötigt:  pip install PyQt5 pyqtgraph pandas numpy

Start:   python flowlog_gui.py               (leeres Fenster, dann CSV laden)
         python flowlog_gui.py flowlog_054.csv
"""

import os
import sys

import numpy as np
import pandas as pd

from PyQt5 import QtCore, QtGui, QtWidgets
import pyqtgraph as pg

pg.setConfigOptions(background="w", foreground="k", antialias=False)

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


# Zeiteinheiten der x-Achse: Faktor bezogen auf Minuten, Nachkommastellen
TIME_UNITS = {
    "min": {"factor": 1.0,      "label": "min", "name": "Minuten", "dec": 1},
    "h":   {"factor": 1 / 60.0, "label": "h",   "name": "Stunden", "dec": 3},
}


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

        self.data = None
        self.lines = {}      # key -> [PlotDataItem je Kanal]
        self.air_lines = {}  # key -> [[InfiniteLine...] je Kanal]
        self.ydata = {}      # key -> [numpy-Array je Kanal] (für Auto Y)
        self.t = None              # Zeitwerte in der aktuell gewählten Einheit
        self.t_min = None          # Zeitwerte in Minuten (Basis)
        self.air_times_min = {}    # Kanal -> Beginn air > 0 in Minuten
        self.time_unit = "min"

        # ---------- Plot-Widgets (pyqtgraph.PlotWidget ist ein QWidget) ----------
        self.plots = {}
        self.legends = {}
        for cfg in PLOTS:
            pw = pg.PlotWidget()
            pw.setTitle(cfg["title"], size="11pt")
            pw.setLabel("left", cfg["ylabel"])
            pw.showGrid(x=True, y=True, alpha=0.3)
            pw.setDownsampling(auto=True, mode="peak")   # schnell bei vielen Punkten
            pw.setClipToView(True)
            pw.hideButtons()                              # eigenes Auto über QPushButtons
            pw.setMenuEnabled(False)
            pw.getAxis("left").setWidth(70)   # gleiche Breite -> x-Achsen exakt bündig
            self.legends[cfg["key"]] = pw.addLegend(offset=(10, 5), colCount=8,
                                                    labelTextSize="8pt")
            self.plots[cfg["key"]] = pw
        self.axes = {k: pw.getPlotItem() for k, pw in self.plots.items()}

        # Gemeinsame Zeitachse (entspricht sharex)
        self.plots["p"].setXLink(self.plots["flow"])
        self.plots["p"].setLabel("bottom", "Zeit [min]")
        self.plots["flow"].getAxis("bottom").setStyle(showValues=False)

        # Eingabefelder bei Zoom/Pan nachführen
        for key, pw in self.plots.items():
            pw.getViewBox().sigRangeChanged.connect(
                lambda vb, rng, *_, k=key: self.on_range_changed(k, rng))
            pw.scene().sigMouseMoved.connect(
                lambda pos, *_, k=key: self.on_mouse_moved(k, pos))

        self.splitter = QtWidgets.QSplitter(QtCore.Qt.Vertical)
        self.splitter.addWidget(self.plots["flow"])
        self.splitter.addWidget(self.plots["p"])

        # ---------- Werkzeugleiste (ersetzt Matplotlib-Toolbar) ----------
        tb = QtWidgets.QToolBar("Werkzeuge")
        tb.setMovable(False)
        style = self.style()
        act_home = tb.addAction(style.standardIcon(QtWidgets.QStyle.SP_BrowserReload),
                                "Gesamtansicht")
        act_home.setToolTip("Gesamtansicht (Auto X + Auto Y)")
        act_home.triggered.connect(self.reset_view)
        tb.addSeparator()
        self.act_pan = tb.addAction("Verschieben")
        self.act_zoom = tb.addAction("Zoom-Rechteck")
        mode_group = QtWidgets.QActionGroup(self)
        for a in (self.act_pan, self.act_zoom):
            a.setCheckable(True)
            mode_group.addAction(a)
        self.act_pan.setChecked(True)
        self.act_pan.setToolTip("Linke Maustaste ziehen = verschieben, Mausrad = zoomen")
        self.act_zoom.setToolTip("Linke Maustaste ziehen = Bereich aufziehen und hineinzoomen")
        mode_group.triggered.connect(self.set_mouse_mode)
        tb.addSeparator()
        act_save = tb.addAction(style.standardIcon(QtWidgets.QStyle.SP_DialogSaveButton),
                                "Als Bild speichern …")
        act_save.triggered.connect(self.save_image)
        self.addToolBar(tb)
        self.statusBar().showMessage("Bereit")

        plot_widget = self.splitter

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
        self.xbox = QtWidgets.QGroupBox()
        xbox = self.xbox
        xl = QtWidgets.QVBoxLayout(xbox)

        # Umschalter Minuten <-> Stunden
        self.btn_unit = QtWidgets.QPushButton()
        self.btn_unit.setCheckable(True)          # gedrückt = Stunden
        self.btn_unit.setToolTip("Zeitachse zwischen Minuten und Stunden umschalten")
        self.btn_unit.toggled.connect(
            lambda checked: self.set_time_unit("h" if checked else "min"))
        xl.addWidget(self.btn_unit)
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
        self.airbox = QtWidgets.QGroupBox()
        airbox = self.airbox
        al = QtWidgets.QVBoxLayout(airbox)
        self.air_text = QtWidgets.QPlainTextEdit()
        self.air_text.setReadOnly(True)
        al.addWidget(self.air_text)
        sl.addWidget(airbox, stretch=1)

        # ---------- Gesamtlayout ----------
        central = QtWidgets.QWidget()
        hl = QtWidgets.QHBoxLayout(central)
        hl.addWidget(plot_widget, stretch=1)

        # Seitenleiste in eine Scroll-Fläche packen: auf kleinen Bildschirmen
        # (z. B. Laptop mit 768 px Höhe) wird sie scrollbar statt das Fenster
        # über den Bildschirmrand hinaus zu vergrößern.
        side_scroll = QtWidgets.QScrollArea()
        side_scroll.setWidget(side)
        side_scroll.setWidgetResizable(True)
        side_scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        side_scroll.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        side_scroll.setFixedWidth(side.width() + 20)   # Platz für vertikale Scrollleiste
        hl.addWidget(side_scroll)
        self.setCentralWidget(central)

        self.update_unit_labels()
        self.fit_to_screen()

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
    def air_onsets(self, ch):
        air = self.data[f"{ch}_air"] > 0
        start = air & ~air.shift(fill_value=False)
        return self.data.loc[start, "time_min"].to_numpy()

    def plot_data(self):
        f = TIME_UNITS[self.time_unit]["factor"]
        self.t_min = self.data["time_min"].to_numpy()
        self.t = self.t_min * f
        self.air_times_min = {ch: self.air_onsets(ch) for ch in CHANNELS}
        air_times = {ch: ts * f for ch, ts in self.air_times_min.items()}

        for cfg in PLOTS:
            key, pw = cfg["key"], self.plots[cfg["key"]]
            pw.clear()
            legend = self.legends[key]
            legend.clear()
            self.lines[key], self.air_lines[key], self.ydata[key] = [], [], []
            for ch, c in zip(CHANNELS, COLORS):
                y = self.data[f"{ch}{cfg['suffix']}"].to_numpy(dtype=float)
                curve = pw.plot(self.t, y, pen=pg.mkPen(c, width=1), connect="finite")
                # Legende mit Platzhalter-Eintrag: ein Klick in die Legende soll
                # nichts am Checkbox-Status vorbei ein-/ausblenden
                legend.addItem(pg.PlotDataItem(pen=pg.mkPen(c, width=2)), ch)
                self.lines[key].append(curve)
                self.ydata[key].append(y)
                vlines = []
                for x in air_times[ch]:
                    vl = pg.InfiniteLine(pos=x, angle=90, movable=False,
                                         pen=pg.mkPen(c, width=2,
                                                      style=QtCore.Qt.DashLine))
                    pw.addItem(vl, ignoreBounds=True)
                    vlines.append(vl)
                self.air_lines[key].append(vlines)
            # Legendeneintrag für die air-Linien
            dummy = pg.PlotDataItem(pen=pg.mkPen("gray", width=1.5, style=QtCore.Qt.DashLine))
            legend.addItem(dummy, "air > 0")

        self.update_air_text()

        for key in self.plots:
            self.update_visibility(key)
        self.reset_view()

    # ------------------------------------------------------------------
    # Interaktion
    # ------------------------------------------------------------------
    def update_visibility(self, key):
        if key not in self.lines:
            return
        ctrl = self.controls[key]
        for i, (curve, vlines) in enumerate(zip(self.lines[key], self.air_lines[key])):
            vis = ctrl.channel_visible(i)
            curve.setVisible(vis)
            for v in vlines:
                v.setVisible(vis and ctrl.air_visible())

    def set_ylim(self, key, lo, hi):
        self.plots[key].setYRange(lo, hi, padding=0)

    def auto_y(self, key):
        """y-Achse an sichtbare Linien im aktuellen x-Bereich anpassen."""
        if key not in self.lines or self.t is None:
            return
        xmin, xmax = self.plots[key].getViewBox().viewRange()[0]
        xmask = (self.t >= xmin) & (self.t <= xmax)
        ymins, ymaxs = [], []
        for curve, y in zip(self.lines[key], self.ydata[key]):
            if not curve.isVisible():
                continue
            yy = y[xmask]
            yy = yy[np.isfinite(yy)]
            if yy.size:
                ymins.append(yy.min())
                ymaxs.append(yy.max())
        if ymins:
            lo, hi = min(ymins), max(ymaxs)
            pad = 0.05 * (hi - lo) if hi > lo else 1.0
            self.set_ylim(key, lo - pad, hi + pad)

    def apply_xlim(self):
        lo, hi = parse_float(self.xmin_edit.text()), parse_float(self.xmax_edit.text())
        if lo is not None and hi is not None and lo < hi:
            self.plots["p"].setXRange(lo, hi, padding=0)   # XLink -> beide Plots

    def auto_x(self):
        if self.t is None:
            return
        self.plots["p"].setXRange(np.nanmin(self.t), np.nanmax(self.t), padding=0)

    def reset_view(self):
        self.auto_x()
        for key in self.plots:
            self.auto_y(key)

    # ------------------------------------------------------------------
    # Fenstergröße an den Bildschirm anpassen
    # ------------------------------------------------------------------
    def fit_to_screen(self):
        """Liest die nutzbare Bildschirmfläche aus (ohne Taskleiste/Dock)
        und setzt das Fenster genau auf diese Größe."""
        screen = QtWidgets.QApplication.screenAt(QtGui.QCursor.pos()) \
            or QtWidgets.QApplication.primaryScreen()
        avail = screen.availableGeometry()

        # Fensterrahmen/Titelleiste abziehen, damit das Fenster inkl. Rahmen passt
        frame_w = self.frameGeometry().width() - self.geometry().width()
        frame_h = self.frameGeometry().height() - self.geometry().height()
        w = avail.width() - frame_w
        h = avail.height() - frame_h

        self.resize(w, h)
        self.move(avail.topLeft())

    # ------------------------------------------------------------------
    # Zeiteinheit Minuten <-> Stunden
    # ------------------------------------------------------------------
    def set_time_unit(self, unit):
        if unit == self.time_unit:
            return
        old_f = TIME_UNITS[self.time_unit]["factor"]
        new_f = TIME_UNITS[unit]["factor"]
        # aktuellen Ausschnitt merken (in Minuten), damit er erhalten bleibt
        x0, x1 = self.plots["p"].getViewBox().viewRange()[0]
        x0_min, x1_min = x0 / old_f, x1 / old_f
        y_ranges = {k: pw.getViewBox().viewRange()[1] for k, pw in self.plots.items()}

        self.time_unit = unit
        self.update_unit_labels()

        if self.t_min is not None:
            self.t = self.t_min * new_f
            for key in self.plots:
                for curve, y in zip(self.lines[key], self.ydata[key]):
                    curve.setData(self.t, y, connect="finite")
                for ch, vlines in zip(CHANNELS, self.air_lines[key]):
                    for vl, x in zip(vlines, self.air_times_min[ch]):
                        vl.setValue(x * new_f)
            self.plots["p"].setXRange(x0_min * new_f, x1_min * new_f, padding=0)
            for key, (y0, y1) in y_ranges.items():
                self.plots[key].setYRange(y0, y1, padding=0)
            self.update_air_text()
        else:
            self.show_xlim(x0_min * new_f, x1_min * new_f)

    def update_unit_labels(self):
        u = TIME_UNITS[self.time_unit]
        other = TIME_UNITS["min" if self.time_unit == "h" else "h"]
        self.plots["p"].setLabel("bottom", f"Zeit [{u['label']}]")
        self.xbox.setTitle(f"Zeitachse [{u['label']}] (beide Plots)")
        self.airbox.setTitle(f"Beginn air > 0 [{u['label']}]")
        self.btn_unit.setText(f"Einheit: {u['name']}  ⇄  {other['name']}")
        self.btn_unit.blockSignals(True)
        self.btn_unit.setChecked(self.time_unit == "h")
        self.btn_unit.blockSignals(False)

    def update_air_text(self):
        u = TIME_UNITS[self.time_unit]
        txt = []
        for ch in CHANNELS:
            ts = self.air_times_min.get(ch, [])
            txt.append(f"{ch}: " + (", ".join(f"{x * u['factor']:.{u['dec']}f}" for x in ts)
                                    if len(ts) else "–"))
        self.air_text.setPlainText("\n".join(txt))

    def show_xlim(self, lo, hi):
        dec = TIME_UNITS[self.time_unit]["dec"]
        self.xmin_edit.setText(f"{lo:.{dec}f}")
        self.xmax_edit.setText(f"{hi:.{dec}f}")

    def on_range_changed(self, key, rng):
        (x0, x1), (y0, y1) = rng
        self.controls[key].show_ylim(y0, y1)
        if key == "p":
            self.show_xlim(x0, x1)

    def on_mouse_moved(self, key, pos):
        vb = self.plots[key].getViewBox()
        if vb.sceneBoundingRect().contains(pos):
            p = vb.mapSceneToView(pos)
            name = "Flow" if key == "flow" else "Druck"
            unit = "µl/min" if key == "flow" else "mbar"
            tu = TIME_UNITS[self.time_unit]
            self.statusBar().showMessage(
                f"{name}:  t = {p.x():.{tu['dec'] + 1}f} {tu['label']}   y = {p.y():.4g} {unit}")

    def set_mouse_mode(self, action):
        mode = pg.ViewBox.RectMode if action is self.act_zoom else pg.ViewBox.PanMode
        for pw in self.plots.values():
            pw.getViewBox().setMouseMode(mode)

    def save_image(self):
        path, _ = QtWidgets.QFileDialog.getSaveFileName(
            self, "Plot als Bild speichern", "flowlog_plot.png",
            "PNG (*.png);;JPEG (*.jpg);;BMP (*.bmp)")
        if path:
            if not self.splitter.grab().save(path):
                QtWidgets.QMessageBox.warning(self, "Fehler", f"Konnte {path} nicht speichern.")
            else:
                self.statusBar().showMessage(f"Gespeichert: {path}", 5000)


def main():
    # Skalierung bei hochauflösenden Bildschirmen (4K, Windows-Skalierung 150 % …)
    QtWidgets.QApplication.setAttribute(QtCore.Qt.AA_EnableHighDpiScaling, True)
    QtWidgets.QApplication.setAttribute(QtCore.Qt.AA_UseHighDpiPixmaps, True)

    app = QtWidgets.QApplication(sys.argv)
    csv_path = sys.argv[1] if len(sys.argv) > 1 else None
    win = MainWindow(csv_path)
    # Maximiert anzeigen: Fenster füllt den Bildschirm (ohne Taskleiste) exakt aus.
    # fit_to_screen() in __init__ sorgt dafür, dass auch das "wiederhergestellte"
    # (nicht maximierte) Fenster nie größer als der Bildschirm ist.
    win.showMaximized()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()