from unittest import TestCase
from helpers import mnemonic_from_dice, DICE_ROLLS_12WORD, DICE_ROLLS_24WORD

# Test vectors from the SeedSigner project - same rolls must give the same
# mnemonic on both devices. Sources:
# - docs/dice_verification.md (the two vectors with a documented entropy value)
# - tests/test_mnemonic_generation.py (the remaining ones)
VECTORS = [
    # 50 rolls -> 12 words
    (
        "65515223131652132161133154444123616466443112153441",
        "hole luggage safe present express tragic orbit shed "
        "switch metal identify path",
    ),
    (
        "12345612345612345612345612345612345612345612345612",
        "unveil nice picture region tragic fault cream strike "
        "tourist control recipe tourist",
    ),
    (
        "1" * 50,
        "diet glad hat rural panther lawsuit act drop "
        "gallery urge where fit",
    ),
    (
        "6" * 50,
        "senior morning song proud recycle toy search apple "
        "trigger lend vibrant arrest",
    ),
    # 99 rolls -> 24 words
    (
        "655152231316521321611331544441236164664431121534415633526456"
        "254462245546236542364246312613322234612",
        "eyebrow obvious such suggest poet seven breeze blame virtual "
        "frown dynamic donor harsh pigeon express broccoli easy apology "
        "scatter force recipe shadow claim radio",
    ),
    (
        "5" + "2" * 44 + "5" * 54,
        "resource timber firm banner horror pupil frozen main pear "
        "direct pioneer broken grid core insane begin sister pony end "
        "debate task silk empty curious",
    ),
    (
        "2" * 45 + "5" * 54,
        "garden uphold level clog sword globe armor issue two cute "
        "scorpion improve verb artwork blind tail raw butter combine "
        "move produce foil feature wave",
    ),
    # last roll changed - must give a completely different mnemonic
    (
        "2" * 45 + "5" * 53 + "6",
        "lizard broken love tired depend eyebrow excess lonely advance "
        "father various cram ignore panic feed plunge miss regret boring "
        "unique galaxy fan detail fly",
    ),
]


class DiceTest(TestCase):
    def test_vectors(self):
        """Same dice rolls as SeedSigner must give the same mnemonic"""
        for rolls, expected in VECTORS:
            self.assertEqual(mnemonic_from_dice(rolls.encode()), expected)

    def test_word_count(self):
        """50 rolls give 12 words, 99 rolls give 24"""
        self.assertEqual(
            len(mnemonic_from_dice(("1" * DICE_ROLLS_12WORD).encode()).split()), 12
        )
        self.assertEqual(
            len(mnemonic_from_dice(("1" * DICE_ROLLS_24WORD).encode()).split()), 24
        )

    def test_bytearray(self):
        """Rolls are collected in a bytearray, not a string"""
        rolls, expected = VECTORS[0]
        self.assertEqual(mnemonic_from_dice(bytearray(rolls.encode())), expected)

    def test_invalid_roll_count(self):
        """Any count other than 50 or 99 is refused instead of silently
        falling back to 24 words like SeedSigner does"""
        for count in [0, 1, 49, 51, 98, 100, 128]:
            with self.assertRaises(RuntimeError):
                mnemonic_from_dice(("1" * count).encode())

    def test_invalid_roll_value(self):
        """Only digits 1-6 are accepted"""
        for bad in ["0", "7", "9", "a", " "]:
            with self.assertRaises(RuntimeError):
                mnemonic_from_dice(("1" * 49 + bad).encode())

    def test_order_matters(self):
        """Rolls must be hashed in the order they were entered"""
        rolls = "65515223131652132161133154444123616466443112153441"
        # micropython has no [::-1], so reverse by hand
        reversed_rolls = "".join([rolls[len(rolls) - 1 - i] for i in range(len(rolls))])
        self.assertNotEqual(
            mnemonic_from_dice(rolls.encode()),
            mnemonic_from_dice(reversed_rolls.encode()),
        )
