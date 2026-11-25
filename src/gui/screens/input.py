"""
Screens where user needs to provide input, 
except mnemonics - they are in mnemonic.py
"""
import lvgl as lv
from ..common import *
from ..decorators import *
from ..components import ButtonMatrix, HintKeyboard
from ..components.modal import confirm_exit
from ..common import add_label
from .screen import Screen
from helpers import DICE_ROLLS_12WORD, DICE_ROLLS_24WORD
import rng


class InputScreen(Screen):
    CHARSET = [
        "1",
        "2",
        "3",
        "4",
        "5",
        "6",
        "7",
        "8",
        "9",
        "0",
        "\n",
        "q",
        "w",
        "e",
        "r",
        "t",
        "y",
        "u",
        "i",
        "o",
        "p",
        "\n",
        "#@",
        "a",
        "s",
        "d",
        "f",
        "g",
        "h",
        "j",
        "k",
        "l",
        "\n",
        lv.SYMBOL.UP,
        "z",
        "x",
        "c",
        "v",
        "b",
        "n",
        "m",
        lv.SYMBOL.LEFT,
        "\n",
        lv.SYMBOL.LEFT + " Back",
        "[    space    ]",
        lv.SYMBOL.OK + " Done",
        "",
    ]
    CHARSET_EXTRA = [
        "!",
        "@",
        "#",
        "$",
        "%",
        "^",
        "&",
        "*",
        "(",
        ")",
        "\n",
        "~",
        "<",
        ">",
        "/",
        "\\",
        "{",
        "}",
        "[",
        "]",
        "\n",
        "aA",
        "\"",
        "'",
        "_",
        "-",
        "=",
        "+",
        "\n",
        "`",
        ":",
        ";",
        ",",
        ".",
        "|",
        "?",
        lv.SYMBOL.LEFT,
        "\n",
        lv.SYMBOL.LEFT + " Back",
        "[    space    ]",
        lv.SYMBOL.OK + " Done",
        "",
    ]

    def __init__(
            self,
            title="Enter your bip-39 password:",
            note="It is never stored on the device",
            suggestion="",
            min_length=0,
            max_length=None,
            strip=False,
    ):
        super().__init__()
        self.title = add_label(title, scr=self, style="title")
        self.min_length = min_length
        self.max_length = max_length
        self.strip = strip

        if note is not None:
            self.note = add_label(note, scr=self, style="hint")
            self.note.align_to(self.title, lv.ALIGN.OUT_BOTTOM_MID, 0, 5)

        self.kb = HintKeyboard(self)
        self.kb.set_map(self.CHARSET)
        self.kb.set_width(HOR_RES)
        self.kb.set_height(int(VER_RES / 2.5))
        self.kb.align(lv.ALIGN.BOTTOM_MID, 0, 0)

        self.ta = lv.textarea(self)
        self.ta.set_text(suggestion)
        self.ta.add_style(styles["ta"], 0)
        self.ta.add_style(styles["ta_cursor"], lv.PART.CURSOR | lv.STATE.FOCUSED)
        self.ta.set_width(HOR_RES - 2 * PADDING)
        self.ta.set_x(PADDING)
        # Keep short inputs (including passphrases) centered in the entry area
        # while the one-line textarea scrolls to follow the cursor when full.
        self.ta.set_style_text_align(lv.TEXT_ALIGN.CENTER, 0)
        self.ta.set_y(PADDING + 150)
        self.ta.set_one_line(True)
        self.ta.set_cursor_pos(lv.TEXTAREA_CURSOR_LAST)
        self.ta.add_state(lv.STATE.FOCUSED)

        self.kb.set_event_cb(self.cb)

        self.warning = add_label("", scr=self, style="hint")
        self.warning.align_to(self.ta, lv.ALIGN.OUT_BOTTOM_MID, 0, 10)

    def cb(self, obj, event):
        if event == lv.EVENT.RELEASED:
            c = obj.get_active_btn_text()
            if c is None:
                return
            if "space" in c:
                c = " "
            if c == lv.SYMBOL.LEFT:
                self.ta.delete_char()
                self.check_text()
            elif c == lv.SYMBOL.UP or c == lv.SYMBOL.DOWN:
                for i, ch in enumerate(self.CHARSET):
                    if ch.isalpha():
                        if c == lv.SYMBOL.UP:
                            self.CHARSET[i] = self.CHARSET[i].upper()
                        else:
                            self.CHARSET[i] = self.CHARSET[i].lower()
                    elif ch == lv.SYMBOL.UP:
                        self.CHARSET[i] = lv.SYMBOL.DOWN
                    elif ch == lv.SYMBOL.DOWN:
                        self.CHARSET[i] = lv.SYMBOL.UP
                self.kb.set_map(self.CHARSET)
            elif c == "#@":
                self.kb.set_map(self.CHARSET_EXTRA)
            elif c == "aA":
                self.kb.set_map(self.CHARSET)
            elif c[0] == lv.SYMBOL.CLOSE:
                self.ta.set_text("")
            elif c[0] == lv.SYMBOL.OK:
                text = self.ta.get_text()
                if not self.check_text():
                    return
                self.ta.set_text("")
                if self.strip:
                    text = text.strip()
                self.set_value(text)
            elif c == lv.SYMBOL.LEFT + " Back":
                self.ta.set_text("")
                self.set_value(None)
            else:
                # check if input is empty:
                self.ta.add_text(c)
                self.check_text()

    def check_text(self):
        text = self.ta.get_text()
        if self.strip:
            text = text.strip()
        # check if input matches the limits
        if len(text) < self.min_length:
            self.warning.set_text("Enter at least %d%s character" % (self.min_length, " non-space" if self.strip else ""))
            return False
        if self.max_length and len(text) > self.max_length:
            self.warning.set_text("Value is too long! Must be between %d and %d characters" % (self.min_length, self.max_length))
            return False
        self.warning.set_text("")
        return True

class PinScreen(Screen):
    network = None
    CANCEL_VALUE = "*"

    def __init__(self, title="Enter your PIN code", note=None, get_word=None, subtitle=None, with_cancel=False):
        super().__init__()
        self.title = add_label(title, scr=self, y=PADDING, style="title")
        if subtitle is not None:
            lbl = add_label(subtitle, scr=self, style="hint")
            lbl.set_recolor(True)
            lbl.align_to(self.title, lv.ALIGN.OUT_BOTTOM_MID, 0, 10)
        if note is not None:
            lbl = add_label(note, scr=self, style="hint")
            lbl.set_style_text_font(lv.font_montserrat_16, 0)
            lbl.set_style_text_letter_space(-1, 0)
            lbl.set_width(HOR_RES)
            lbl.set_x(0)
            lbl.align_to(self.title, lv.ALIGN.OUT_BOTTOM_MID, 0, 90)
        self.get_word = get_word
        if get_word is not None:
            self.words = add_label(get_word(b""), scr=self, style="title")
            self.words.set_style_text_font(lv.font_montserrat_22, 0)
            self.words.align_to(self.title, lv.ALIGN.OUT_BOTTOM_MID, 0, 130 if note is not None else 120)
        btnm = ButtonMatrix(self)
        # shuffle numbers to make sure
        # no constant fingerprints left on screen
        buttons = ["%d" % i for i in range(0, 10)]
        btnmap = []
        for j in range(3):
            for i in range(3):
                v = rng.get_random_bytes(1)[0] % len(buttons)
                btnmap.append(buttons.pop(v))
            btnmap.append("\n")
        btnmap = btnmap + [lv.SYMBOL.CLOSE, buttons.pop(), " ", ""]
        btnm.set_map(btnmap)
        btnm.set_btn_ctrl(11, ButtonMatrix.CTRL.HIDDEN | ButtonMatrix.CTRL.INACTIVE)
        btnm.set_width(HOR_RES)
        btnm.set_height(HOR_RES)
        btnm.align(lv.ALIGN.BOTTOM_MID, 0, -100)
        btnm.add_style(styles["btnm_bg"], 0)
        btnm.add_style(styles["btnm"], lv.PART.ITEMS)
        # Keep pressed and released states visually identical to avoid sidechannels.
        btnm.add_style(styles["btnm_pressed"], lv.PART.ITEMS | lv.STATE.PRESSED)
        if PIN_KEYPAD_FONT is not None:
            btnm.set_style_text_font(PIN_KEYPAD_FONT, lv.PART.ITEMS)
        else:
            btnm.set_style_text_font(lv.font_montserrat_28, lv.PART.ITEMS)
            btnm.set_style_transform_scale(FONT_SCALE_MINUS_TWO_PT, lv.PART.ITEMS)

        self.pin = lv.textarea(self)
        self.pin.set_text("")
        self.pin.set_password_mode(True)
        self.pin.add_style(styles["ta"], 0)
        self.pin.set_style_text_font(lv.font_montserrat_28, 0)
        self.pin.set_style_text_color(styles["ctxt"], 0)
        self.pin.set_style_text_letter_space(15, 0)
        self.pin.set_width(HOR_RES - 2 * PADDING)
        self.pin.set_x(PADDING)
        self.pin.set_y(PADDING + 50)
        self.pin.set_style_opa(lv.OPA.TRANSP, lv.PART.CURSOR)
        self.pin.set_one_line(True)
        self.pin.set_style_text_align(lv.TEXT_ALIGN.CENTER, 0)
        self.pin.set_password_show_time(0)
        self.pin.align_to(btnm, lv.ALIGN.OUT_TOP_MID, 0, -80)

        self.next_button = add_button(scr=self, callback=on_release(self.submit))

        self.next_label = add_button_label(self.next_button, "Next " + lv.SYMBOL.RIGHT)

        if with_cancel:
            self.cancel_button = add_button(scr=self, callback=on_release(self.cancel))

            self.cancel_label = add_button_label(self.cancel_button, lv.SYMBOL.LEFT + " Cancel")

            align_button_pair(self.cancel_button, self.next_button)
            center_button_label(self.cancel_label)
            center_button_label(self.next_label)

        btnm.set_event_cb(self.cb)


    def reset(self):
        self.pin.set_text("")
        if self.get_word is not None:
            self.words.set_text(self.get_word(b""))

    def cb(self, obj, event):
        if event == lv.EVENT.RELEASED:
            c = obj.get_active_btn_text()
            if c is None or c == " ":
                return
            if c == lv.SYMBOL.CLOSE:
                self.reset()
            else:
                self.pin.add_text(c)
                # add new anti-phishing word
                if self.get_word is not None:
                    cur_words = self.words.get_text()
                    cur_words += " " + self.get_word(self.pin.get_text())
                    self.words.set_text(cur_words)

    def get_value(self):
        v = self.pin.get_text()
        return None if v == self.CANCEL_VALUE else v

    def submit(self):
        self.release()

    def cancel(self):
        # obj.del_async()
        self.pin.set_text(self.CANCEL_VALUE)
        self.release()

class DerivationScreen(Screen):
    PATH_CHARSET = [
        "1",
        "2",
        "3",
        lv.SYMBOL.LEFT,
        "\n",
        "4",
        "5",
        "6",
        "h",
        "\n",
        "7",
        "8",
        "9",
        "/",
        "\n",
        "Back",
        "0",
        lv.SYMBOL.CLOSE,
        lv.SYMBOL.OK,
        "",
    ]

    def __init__(self, title="Enter derivation path"):
        super().__init__()
        self.title = add_label(title, scr=self, y=PADDING, style="title")
        self.kb = ButtonMatrix(self)
        self.kb.set_map(self.PATH_CHARSET)
        self.kb.set_width(HOR_RES)
        self.kb.set_height(VER_RES // 2)
        self.kb.align(lv.ALIGN.BOTTOM_MID, 0, 0)

        lbl = add_label("m/", style="title", scr=self)
        lbl.set_y(PADDING + 150)
        lbl.set_width(40)
        lbl.set_x(PADDING)

        self.ta = lv.textarea(self)
        self.ta.set_text("")
        self.ta.add_style(styles["ta"], 0)
        self.ta.set_width(HOR_RES - 2 * PADDING - 40)
        self.ta.set_x(PADDING + 40)
        self.ta.set_y(PADDING + 150)
        self.ta.set_style_opa(lv.OPA.TRANSP, lv.PART.CURSOR)
        self.ta.set_one_line(True)

        self.kb.set_event_cb(self.cb)

    def cb(self, obj, event):
        if event != lv.EVENT.RELEASED:
            return
        c = obj.get_active_btn_text()
        if c is None:
            return
        der = self.ta.get_text()
        if len(der) == 0:
            last = "/"
        else:
            last = der[-1]
        if c == "Back":
            self.ta.set_text("")
            self.set_value(None)
        if c[0] == lv.SYMBOL.LEFT:
            self.ta.delete_char()
        elif c[0] == lv.SYMBOL.CLOSE:
            self.ta.set_text("")
        elif c[0] == lv.SYMBOL.OK:
            self.set_value("m/" + self.ta.get_text())
            self.ta.set_text("")
        elif c[0] == "h":
            if last.isdigit():
                self.ta.add_text("h/")
        elif c[0] == "/":
            if last.isdigit() or last == "h":
                self.ta.add_text(c)
        else:
            self.ta.add_text(c)


class NumericScreen(Screen):
    NUMERIC_CHARSET = [
        "1",
        "2",
        "3",
        "\n",
        "4",
        "5",
        "6",
        "\n",
        "7",
        "8",
        "9",
        "\n",
        lv.SYMBOL.LEFT,
        "0",
        lv.SYMBOL.OK,
        "",
    ]

    def __init__(
            self,
            title="Enter account number",
            note=None,
            current_val='0'
    ):
        super().__init__()
        if note is None:
            note = "Current account number: %s" % current_val
        self.title = add_label(title, scr=self, y=PADDING, style="title")

        self.note = add_label(note, scr=self, style="hint")
        self.note.align_to(self.title, lv.ALIGN.OUT_BOTTOM_MID, 0, 5)

        self.kb = ButtonMatrix(self)
        self.kb.set_map(self.NUMERIC_CHARSET)
        self.kb.set_width(HOR_RES)
        self.kb.set_height(VER_RES // 2)
        self.kb.align(lv.ALIGN.BOTTOM_MID, 0, 0)

        lbl = add_label('', style="title", scr=self)
        lbl.set_y(PADDING + 150)
        lbl.set_width(40)
        lbl.set_x(PADDING)

        self.ta = lv.textarea(self)
        self.ta.set_text("")
        self.ta.add_style(styles["ta"], 0)
        self.ta.set_width(HOR_RES - 2 * PADDING - 40)
        self.ta.set_x(PADDING + 40)
        self.ta.set_y(PADDING + 150)
        self.ta.set_style_opa(lv.OPA.TRANSP, lv.PART.CURSOR)
        self.ta.set_one_line(True)
        self.kb.set_event_cb(self.cb)

    def cb(self, obj, event):
        if event != lv.EVENT.RELEASED:
            return
        c = obj.get_active_btn_text()
        if c is None:
            return
        account = self.ta.get_text()
        if len(account) == 0:
            last = '0'
        else:
            last = account[-1]
        if c[0] == lv.SYMBOL.LEFT:
            self.ta.delete_char()
        elif c[0] == lv.SYMBOL.OK:
            self.set_value(self.ta.get_text())
            self.ta.set_text("")
        else:
            self.ta.add_text(c)


class DiceScreen(Screen):
    """
    Collects dice rolls to generate a mnemonic from them.
    Returns a bytearray with one ASCII digit "1".."6" per roll,
    or None if the user cancelled.
    """

    # dice faces as (column, row) of the pips in a 3x3 grid
    PIPS = {
        1: [(1, 1)],
        2: [(0, 0), (2, 2)],
        3: [(0, 0), (1, 1), (2, 2)],
        4: [(0, 0), (2, 0), (0, 2), (2, 2)],
        5: [(0, 0), (2, 0), (1, 1), (0, 2), (2, 2)],
        6: [(0, 0), (0, 1), (0, 2), (2, 0), (2, 1), (2, 2)],
    }
    DICE_W = 142
    DICE_H = 132
    DICE_GAP = 18
    DICE_Y = 300
    PIP_SIZE = 20
    UNDO_H = 64
    # gap between the dice and undo, larger than the gap between the dice
    # themselves so a low tap on "5" does not remove a roll
    UNDO_GAP = 28
    # rolls per line, so 50 rolls are exactly one line
    ROLLS_PER_LINE = 50
    ROLLS_PER_GROUP = 5

    def __init__(
        self,
        num_rolls,
        title="Dice roll 1/%d",
        note="Roll your dice and tap what you rolled.\n"
        "Keep the screen out of sight.",
    ):
        super().__init__()
        if num_rolls not in (DICE_ROLLS_12WORD, DICE_ROLLS_24WORD):
            raise RuntimeError("Invalid number of dice rolls")
        self.num_rolls = num_rolls
        # fixed-size buffer so it is never reallocated - a growing bytearray
        # would leave copies of the rolls behind on the heap
        self.rolls = bytearray(num_rolls)
        self.count = 0
        self.modal = None

        self.title = add_label(title % num_rolls, scr=self, y=PADDING, style="title")

        self.bar = lv.bar(self)
        self.bar.set_size(HOR_RES - 2 * PADDING, 8)
        self.bar.set_range(0, num_rolls)
        self.bar.set_value(0, False)
        self.bar.set_pos(PADDING, 85)

        # rolls entered so far, grouped in fives so they can be checked
        # against a written record
        self.sequence_style = lv.style_t()
        self.sequence_style.init()
        self.sequence_style.set_text_font(lv.font_montserrat_16)
        self.sequence_style.set_text_color(styles["ctxt"])
        # 50 rolls in groups of five are 59 characters and only fit with a
        # slightly tighter letter spacing
        self.sequence_style.set_text_letter_space(-1)
        self.sequence_style.set_text_line_space(8)
        self.sequence = lv.label(self)
        self.sequence.add_style(self.sequence_style, 0)
        self.sequence.set_long_mode(lv.label.LONG_MODE.WRAP)
        self.sequence.set_width(HOR_RES - 2 * PADDING)
        # left aligned, so the digits stay put as the line grows and both
        # lines line up with each other
        self.sequence.set_style_text_align(lv.TEXT_ALIGN.LEFT, 0)
        self.sequence.set_text("")
        self.sequence.set_pos(PADDING, 110)

        self.note = add_label(note, scr=self, y=190, style="hint")

        self.pip_style = lv.style_t()
        self.pip_style.init()
        self.pip_style.set_bg_color(styles["ctxt"])
        self.pip_style.set_bg_opa(255)
        self.pip_style.set_radius(self.PIP_SIZE // 2)
        self.pip_style.set_border_width(0)
        self.pip_style.set_outline_width(0)
        self.pip_style.set_shadow_width(0)
        self.pip_style.set_pad_all(0)

        self.dice_style = lv.style_t()
        self.dice_style.init()
        self.dice_style.set_radius(20)

        # dimmed while a button can not be used
        self.disabled_style = lv.style_t()
        self.disabled_style.init()
        self.disabled_style.set_opa(lv.OPA._40)

        # dice buttons, 3 columns x 2 rows
        # separate buttons instead of a button matrix: a matrix has no
        # spacing between its keys, so a touch that lands slightly off hits
        # the neighbouring value. Here it lands between two buttons and does
        # nothing at all, which the counter shows. A wrong roll would
        # otherwise go unnoticed - unlike a mistyped PIN.
        # The keys are not shuffled and keep their press feedback, unlike
        # PinScreen: 50-99 taps cover all six keys anyway, so the smudge
        # pattern carries no information, and the rolls are on screen in
        # plain sight regardless.
        x0 = (HOR_RES - 3 * self.DICE_W - 2 * self.DICE_GAP) // 2
        self.dice = []
        for value in range(1, 7):
            col = (value - 1) % 3
            row = (value - 1) // 3
            btn = add_button(
                scr=self,
                callback=on_release(cb_with_args(self.add_roll, value)),
            )
            btn.add_style(self.dice_style, 0)
            btn.add_style(self.dice_style, lv.STATE.PRESSED)
            btn.add_style(self.disabled_style, lv.STATE.DISABLED)
            btn.set_align(lv.ALIGN.DEFAULT)
            btn.set_size(self.DICE_W, self.DICE_H)
            btn.set_pos(
                x0 + col * (self.DICE_W + self.DICE_GAP),
                self.DICE_Y + row * (self.DICE_H + self.DICE_GAP),
            )
            self.add_pips(btn, value)
            self.dice.append(btn)

        self.undo_button = add_button(scr=self, callback=on_release(self.undo))
        self.undo_button.add_style(self.disabled_style, lv.STATE.DISABLED)
        self.undo_button.set_align(lv.ALIGN.DEFAULT)
        self.undo_button.set_size(self.DICE_W, self.UNDO_H)
        self.undo_button.set_pos(
            (HOR_RES - self.DICE_W) // 2,
            self.DICE_Y + 2 * self.DICE_H + self.DICE_GAP + self.UNDO_GAP,
        )
        add_button_label(self.undo_button, lv.SYMBOL.LEFT + " Undo")

        self.cancel_button = add_button(scr=self, callback=on_release(self.cancel))
        self.cancel_label = add_button_label(self.cancel_button, lv.SYMBOL.LEFT + " Cancel")
        self.done_button = add_button(scr=self, callback=on_release(self.confirm))
        self.done_button.add_style(self.disabled_style, lv.STATE.DISABLED)
        self.done_label = add_button_label(self.done_button, lv.SYMBOL.OK + " Done")
        align_button_pair(self.cancel_button, self.done_button)
        center_button_label(self.cancel_label)
        center_button_label(self.done_label)

        self.update()

    def add_pips(self, btn, value):
        """Draws the pips of one die face"""
        cx = [self.DICE_W // 4, self.DICE_W // 2, 3 * self.DICE_W // 4]
        cy = [self.DICE_H // 4, self.DICE_H // 2, 3 * self.DICE_H // 4]
        for col, row in self.PIPS[value]:
            pip = lv.obj(btn)
            pip.remove_style_all()
            pip.add_style(self.pip_style, 0)
            pip.set_size(self.PIP_SIZE, self.PIP_SIZE)
            pip.set_pos(cx[col] - self.PIP_SIZE // 2, cy[row] - self.PIP_SIZE // 2)
            pip.remove_flag(lv.obj.FLAG.CLICKABLE)
            pip.remove_flag(lv.obj.FLAG.SCROLLABLE)

    def format_rolls(self):
        """Rolls in groups of five, ROLLS_PER_LINE per line"""
        lines = []
        for start in range(0, self.count, self.ROLLS_PER_LINE):
            line = self.rolls[start : min(start + self.ROLLS_PER_LINE, self.count)]
            lines.append(
                " ".join(
                    [
                        line[i : i + self.ROLLS_PER_GROUP].decode()
                        for i in range(0, len(line), self.ROLLS_PER_GROUP)
                    ]
                )
            )
        return "\n".join(lines)

    @staticmethod
    def set_enabled(btn, enabled):
        if enabled:
            btn.remove_state(lv.STATE.DISABLED)
        else:
            btn.add_state(lv.STATE.DISABLED)

    def update(self):
        """
        Updates counter, progress bar, rolls and button states.

        The checks in add_roll(), undo() and confirm() enforce the roll
        count on their own, disabling the buttons is not relied upon.
        """
        done = self.count == self.num_rolls
        if done:
            self.title.set_text("All %d rolls entered" % self.num_rolls)
        else:
            self.title.set_text("Dice roll %d/%d" % (self.count + 1, self.num_rolls))
        self.bar.set_value(self.count, False)
        self.sequence.set_text(self.format_rolls())
        for btn in self.dice:
            self.set_enabled(btn, not done)
        self.set_enabled(self.done_button, done)
        self.set_enabled(self.undo_button, self.count > 0)

    def add_roll(self, value):
        if self.count >= self.num_rolls:
            return
        self.rolls[self.count] = 0x30 + value  # "1".."6"
        self.count += 1
        self.update()

    def undo(self):
        if self.count == 0:
            return
        self.count -= 1
        self.rolls[self.count] = 0
        self.update()

    def wipe(self):
        """Overwrites the rolls - they are the seed"""
        for i in range(len(self.rolls)):
            self.rolls[i] = 0
        self.count = 0
        self.update()

    def confirm(self):
        if self.count != self.num_rolls:
            return
        # the caller derives from the rolls and overwrites them afterwards,
        # so only the display is cleared here
        self.sequence.set_text("")
        self.set_value(self.rolls)

    def cancel(self):
        if self.count == 0:
            self.set_value(None)
            return
        if self.modal is not None:
            return
        self.modal = confirm_exit(
            self,
            "Are you sure you want to exit?\n\n"
            "All %d dice rolls will be forgotten!" % self.count,
            self.discard,
        )
        # forget the modal once it is closed with "stay"
        self.modal.add_event_cb(lambda e: self.forget_modal(), lv.EVENT.DELETE, None)

    def forget_modal(self):
        self.modal = None

    def discard(self):
        self.wipe()
        self.set_value(None)
