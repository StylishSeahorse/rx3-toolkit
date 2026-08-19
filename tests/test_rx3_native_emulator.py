import unittest

from tools.rx3_native_emulator.cli import Deck, Rx3State


class NativeModelTests(unittest.TestCase):
    def test_playback_advances_only_when_playing(self):
        deck = Deck("DECK 1")
        deck.tick(2)
        self.assertEqual(deck.position, 0)
        deck.playing = True
        deck.tick(2)
        self.assertGreater(deck.position, 0)

    def test_beat_jump_uses_128_bpm_grid(self):
        deck = Deck("DECK 1", beat_jump=32)
        deck.jump(32)
        self.assertEqual(deck.position, 15.0)

    def test_active_deck_is_selectable(self):
        state = Rx3State()
        state.active_deck = 1
        self.assertEqual(state.active().name, "DECK 2")


if __name__ == "__main__":
    unittest.main()
