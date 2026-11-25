from unittest import TestCase
from binascii import unhexlify
from embit import bip39
from helpers import mnemonic_from_dice, DICE_ROLLS_12WORD, DICE_ROLLS_24WORD

# The two vectors SeedSigner documents with their entropy value, so we can
# pin the entropy and not only the words (docs/dice_verification.md)
DOC_VECTOR_50 = "65515223131652132161133154444123616466443112153441"
DOC_ENTROPY_50 = "6cb09af855050dcde6fe2adc3181c250"
DOC_MNEMONIC_50 = (
    "hole luggage safe present express tragic orbit shed "
    "switch metal identify path"
)
DOC_VECTOR_99 = (
    "655152231316521321611331544441236164664431121534415633526456"
    "254462245546236542364246312613322234612"
)
DOC_ENTROPY_99 = "51531761ec7a738946e0b9f46bb11320a695495430e345c14f01ad8b3b898a6d"
DOC_MNEMONIC_99 = (
    "eyebrow obvious such suggest poet seven breeze blame virtual "
    "frown dynamic donor harsh pigeon express broccoli easy apology "
    "scatter force recipe shadow claim radio"
)

# Test vectors from the SeedSigner project - same rolls must give the same
# mnemonic on both devices. Sources:
# - docs/dice_verification.md (the two vectors with a documented entropy value)
# - tests/test_mnemonic_generation.py (the remaining ones)
VECTORS = [
    # 50 rolls -> 12 words
    (DOC_VECTOR_50, DOC_MNEMONIC_50),
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
    (DOC_VECTOR_99, DOC_MNEMONIC_99),
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
        for count in [0, 1, 6, 49, 51, 98, 100, 128]:
            with self.assertRaises(RuntimeError):
                mnemonic_from_dice(("1" * count).encode())

    def test_invalid_roll_value(self):
        """Only digits 1-6 are accepted"""
        for bad in ["0", "7", "9", "a", " "]:
            with self.assertRaises(RuntimeError):
                mnemonic_from_dice(("1" * 49 + bad).encode())

    def test_order_matters(self):
        """Rolls must be hashed in the order they were entered"""
        rolls = DOC_VECTOR_50
        # micropython has no [::-1], so reverse by hand
        reversed_rolls = "".join([rolls[len(rolls) - 1 - i] for i in range(len(rolls))])
        self.assertNotEqual(
            mnemonic_from_dice(rolls.encode()),
            mnemonic_from_dice(reversed_rolls.encode()),
        )


class DiceEntropyTest(TestCase):
    """
    Pins the entropy, not only the words. Both values are published in
    SeedSigner's docs/dice_verification.md. This also catches a wrong slice
    of the hash - the last 16 bytes instead of the first 16, for example.
    """

    def test_entropy_50_rolls(self):
        entropy = unhexlify(DOC_ENTROPY_50)
        self.assertEqual(len(entropy), 16)
        self.assertEqual(bip39.mnemonic_from_bytes(entropy), DOC_MNEMONIC_50)
        self.assertEqual(mnemonic_from_dice(DOC_VECTOR_50.encode()), DOC_MNEMONIC_50)

    def test_entropy_99_rolls(self):
        entropy = unhexlify(DOC_ENTROPY_99)
        self.assertEqual(len(entropy), 32)
        self.assertEqual(bip39.mnemonic_from_bytes(entropy), DOC_MNEMONIC_99)
        self.assertEqual(mnemonic_from_dice(DOC_VECTOR_99.encode()), DOC_MNEMONIC_99)

    def test_entropy_round_trip(self):
        self.assertEqual(
            bip39.mnemonic_to_bytes(mnemonic_from_dice(DOC_VECTOR_50.encode())),
            unhexlify(DOC_ENTROPY_50),
        )
        self.assertEqual(
            bip39.mnemonic_to_bytes(mnemonic_from_dice(DOC_VECTOR_99.encode())),
            unhexlify(DOC_ENTROPY_99),
        )

    def test_generated_mnemonics_are_valid(self):
        """Every derived mnemonic must pass the BIP39 checksum"""
        for rolls in [
            DOC_VECTOR_50,
            DOC_VECTOR_99,
            "1" * DICE_ROLLS_12WORD,
            "6" * DICE_ROLLS_12WORD,
            "1" * DICE_ROLLS_24WORD,
            "6" * DICE_ROLLS_24WORD,
        ]:
            mnemonic = mnemonic_from_dice(rolls.encode())
            self.assertTrue(bip39.mnemonic_is_valid(mnemonic), mnemonic)


class DiceRejectedInputTest(TestCase):
    def test_coldcard_6_roll_vector_is_rejected(self):
        """
        SeedSigner turns any length other than 50 into 24 words, so its test
        suite has a 6 roll vector from the ColdCard docs. We only allow 50
        and 99 rolls - a deliberate difference, pinned here so nobody
        "fixes" it by accident.
        """
        with self.assertRaises(RuntimeError):
            mnemonic_from_dice(b"123456")

    def test_invalid_value_anywhere_in_the_sequence(self):
        """A bad byte is caught no matter where it is"""
        for pos in [0, 1, 25, 48, 49]:
            rolls = bytearray(b"1" * DICE_ROLLS_12WORD)
            rolls[pos] = ord("0")
            with self.assertRaises(RuntimeError):
                mnemonic_from_dice(rolls)
            rolls[pos] = ord("7")
            with self.assertRaises(RuntimeError):
                mnemonic_from_dice(rolls)

    def test_wiped_buffer_is_rejected(self):
        """Once the caller overwrote the rolls they must not derive anything"""
        for count in [DICE_ROLLS_12WORD, DICE_ROLLS_24WORD]:
            with self.assertRaises(RuntimeError):
                mnemonic_from_dice(bytearray(count))

    def test_string_input_is_not_silently_accepted(self):
        """
        The function takes a bytes-like object. A str must fail loudly
        rather than hash something else.
        """
        try:
            result = mnemonic_from_dice(DOC_VECTOR_50)
        except Exception:
            return
        self.fail("str input accepted: %s" % result)


class DiceSensitivityTest(TestCase):
    def test_single_roll_change_changes_everything(self):
        """One different roll must give a completely different mnemonic"""
        base = mnemonic_from_dice(DOC_VECTOR_50.encode())
        for pos in [0, 25, 49]:
            rolls = bytearray(DOC_VECTOR_50.encode())
            rolls[pos] = ord("1") if rolls[pos] != ord("1") else ord("2")
            other = mnemonic_from_dice(rolls)
            self.assertNotEqual(base, other)
            same = 0
            for a, b in zip(base.split(), other.split()):
                if a == b:
                    same += 1
            self.assertTrue(same <= 2, "%d words unchanged" % same)

    def test_12_and_24_words_from_the_same_prefix(self):
        """
        The 50 roll prefix of the 99 roll vector must not produce the first
        12 words of the 24 word mnemonic
        """
        m12 = mnemonic_from_dice(DOC_VECTOR_99[:50].encode())
        m24 = mnemonic_from_dice(DOC_VECTOR_99.encode())
        self.assertNotEqual(m12.split()[0], m24.split()[0])
