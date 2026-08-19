"""A dependency-free, host-native RX3 control-surface emulator.

The emulator deliberately keeps the model separate from Tk.  This makes the
feature behavior testable on CI and gives contributors a usable PC screen
without Docker, QEMU, or proprietary firmware files.
"""

from __future__ import annotations

import argparse
import math
import pathlib
import sys
import tkinter as tk
from dataclasses import dataclass, field
from tkinter import ttk


@dataclass
class Deck:
    name: str
    track: str = "No track loaded"
    playing: bool = False
    position: float = 0.0
    pitch: float = 0.0
    key: int = 0
    instrumental: bool = True
    vocal: bool = True
    beat_jump: int = 4
    cue: float = 0.0

    def tick(self, seconds: float) -> None:
        if self.playing:
            self.position = (self.position + seconds * (1 + self.pitch / 100)) % 240

    def jump(self, beats: int) -> None:
        self.position = max(0.0, self.position + beats * 60 / 128)


@dataclass
class Rx3State:
    active_deck: int = 0
    tab: str = "PERFORMANCE"
    decks: list[Deck] = field(default_factory=lambda: [Deck("DECK 1"), Deck("DECK 2")])

    def active(self) -> Deck:
        return self.decks[self.active_deck]


class Rx3Emulator(tk.Tk):
    WIDTH, HEIGHT = 1280, 720
    BG = "#101114"
    PANEL = "#1b1d22"
    GRID = "#30343c"
    TEXT = "#f1f3f5"
    MUTED = "#a5abb5"
    ORANGE = "#ff5b21"

    def __init__(self, state: Rx3State | None = None) -> None:
        super().__init__()
        self.state = state or Rx3State()
        self.title("XDJ-RX3 — host-native emulator")
        self.geometry("1280x760")
        self.minsize(900, 560)
        self.configure(background=self.BG)
        self.canvas = tk.Canvas(self, background=self.BG, highlightthickness=0)
        self.canvas.pack(fill="both", expand=True)
        self.canvas.bind("<Button-1>", self._click)
        self.bind("<space>", lambda _event: self.toggle_play())
        self.bind("<Left>", lambda _event: self.state.active().jump(-self.state.active().beat_jump))
        self.bind("<Right>", lambda _event: self.state.active().jump(self.state.active().beat_jump))
        self.bind("<KeyPress-1>", lambda _event: self.select_deck(0))
        self.bind("<KeyPress-2>", lambda _event: self.select_deck(1))
        self._last_ms = self.winfo_toplevel().after(40, self._tick)
        self.protocol("WM_DELETE_WINDOW", self.destroy)
        self._draw()

    def _tick(self) -> None:
        for deck in self.state.decks:
            deck.tick(0.04)
        self._draw()
        self._last_ms = self.after(40, self._tick)

    def select_deck(self, index: int) -> None:
        self.state.active_deck = index
        self._draw()

    def toggle_play(self) -> None:
        self.state.active().playing = not self.state.active().playing
        self._draw()

    def _text(self, x: float, y: float, value: str, size: int = 12, fill: str | None = None, anchor: str = "nw") -> None:
        self.canvas.create_text(x, y, text=value, fill=fill or self.TEXT, font=("Segoe UI", size), anchor=anchor)

    def _button(self, x1: float, y1: float, x2: float, y2: float, label: str, tag: str, active: bool = False) -> None:
        self.canvas.create_rectangle(x1, y1, x2, y2, fill=self.ORANGE if active else self.PANEL, outline=self.GRID, tags=(tag,))
        self._text((x1 + x2) / 2, (y1 + y2) / 2, label, 12, anchor="center")

    def _draw(self) -> None:
        self.canvas.delete("all")
        width = max(self.WIDTH, self.winfo_width())
        height = max(self.HEIGHT, self.winfo_height() - 4)
        self.canvas.create_rectangle(0, 0, width, height, fill=self.BG, outline="")
        self._text(24, 18, "XDJ-RX3", 20)
        self._text(24, 48, "HOST-NATIVE EMULATOR  •  BEHAVIORAL MODEL", 10, self.MUTED)
        self._text(width - 24, 22, "Firmware execution: QEMU mode", 10, self.MUTED, "ne")
        self._button(220, 16, 390, 52, "PERFORMANCE", "tab-performance", self.state.tab == "PERFORMANCE")
        self._button(398, 16, 520, 52, "KEY", "tab-key", self.state.tab == "KEY")
        self._button(528, 16, 670, 52, "STEMS", "tab-stems", self.state.tab == "STEMS")

        margin, gap = 24, 18
        deck_width = (width - margin * 2 - gap) / 2
        for index, deck in enumerate(self.state.decks):
            x = margin + index * (deck_width + gap)
            self._draw_deck(x, 86, deck_width, height - 185, deck, index == self.state.active_deck)
        self._text(24, height - 76, "Keys: 1/2 select deck   Space play/pause   ←/→ beat jump   Mouse: touch controls", 11, self.MUTED)
        self._text(width - 24, height - 76, "No audio output in behavioral mode", 11, self.MUTED, "ne")

    def _draw_deck(self, x: float, y: float, w: float, h: float, deck: Deck, selected: bool) -> None:
        self.canvas.create_rectangle(x, y, x + w, y + h, fill=self.PANEL, outline=self.ORANGE if selected else self.GRID, width=2)
        self._text(x + 18, y + 16, deck.name, 16, self.ORANGE if selected else self.TEXT)
        self._text(x + w - 18, y + 18, "PLAYING" if deck.playing else "PAUSED", 10, "#70e08a" if deck.playing else self.MUTED, "ne")
        self._text(x + 18, y + 52, deck.track, 12, self.MUTED)
        wave_y, wave_h = y + 84, 106
        self.canvas.create_rectangle(x + 18, wave_y, x + w - 18, wave_y + wave_h, fill="#111318", outline=self.GRID)
        points = []
        for i in range(161):
            px = x + 20 + i * (w - 40) / 160
            value = math.sin(i * .34) * .32 + math.sin(i * .083) * .22
            points.extend((px, wave_y + wave_h / 2 + value * wave_h))
        self.canvas.create_line(*points, fill="#6d7785", width=1)
        marker = x + 20 + (deck.position % 60) / 60 * (w - 40)
        self.canvas.create_line(marker, wave_y, marker, wave_y + wave_h, fill=self.ORANGE, width=2)
        mins, secs = divmod(int(deck.position), 60)
        self._text(x + 18, wave_y + wave_h + 12, f"{mins:02d}:{secs:02d}", 24)
        self._text(x + w - 18, wave_y + wave_h + 20, f"KEY {deck.key:+d}   PITCH {deck.pitch:+.1f}%", 11, self.MUTED, "ne")
        by = y + h - 72
        self._button(x + 18, by, x + 130, by + 44, "CUE", f"cue-{deck.name}")
        self._button(x + 140, by, x + 252, by + 44, "PLAY", f"play-{deck.name}", deck.playing)
        self._button(x + 262, by, x + 374, by + 44, f"JUMP {deck.beat_jump}", f"jump-{deck.name}")
        if self.state.tab == "KEY":
            self._button(x + w - 190, by, x + w - 132, by + 44, "−", f"key-minus-{deck.name}")
            self._button(x + w - 126, by, x + w - 68, by + 44, "+", f"key-plus-{deck.name}")
        if self.state.tab == "STEMS":
            self._button(x + 18, by - 58, x + 130, by - 14, "VOCAL", f"vocal-{deck.name}", deck.vocal)
            self._button(x + 140, by - 58, x + 252, by - 14, "INST", f"inst-{deck.name}", deck.instrumental)

    def _click(self, event: tk.Event[tk.Misc]) -> None:
        tag = self.canvas.gettags("current")
        if not tag:
            return
        name = tag[0]
        if name.startswith("tab-"):
            self.state.tab = name[4:].upper()
        elif name.startswith("play-"):
            self.select_deck(0 if name.endswith("1") else 1); self.toggle_play()
        elif name.startswith("jump-"):
            self.select_deck(0 if name.endswith("1") else 1); self.state.active().jump(self.state.active().beat_jump)
        elif name.startswith("key-minus-"):
            self.select_deck(0 if name.endswith("1") else 1); self.state.active().key = max(-12, self.state.active().key - 1)
        elif name.startswith("key-plus-"):
            self.select_deck(0 if name.endswith("1") else 1); self.state.active().key = min(12, self.state.active().key + 1)
        elif name.startswith("vocal-"):
            self.select_deck(0 if name.endswith("1") else 1); self.state.active().vocal = not self.state.active().vocal
        elif name.startswith("inst-"):
            self.select_deck(0 if name.endswith("1") else 1); self.state.active().instrumental = not self.state.active().instrumental
        self._draw()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run the host-native RX3 behavioral emulator")
    parser.add_argument("--track", action="append", default=[], help="label a track for deck 1 or 2")
    args = parser.parse_args(argv)
    state = Rx3State()
    for index, track in enumerate(args.track[:2]):
        state.decks[index].track = pathlib.Path(track).stem
    Rx3Emulator(state).mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
