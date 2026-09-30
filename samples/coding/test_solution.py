from solution import to_roman


def test_simple():
    assert to_roman(3) == "III"
    assert to_roman(58) == "LVIII"


def test_subtractive():
    assert to_roman(4) == "IV"
    assert to_roman(9) == "IX"
    assert to_roman(1994) == "MCMXCIV"


def test_largest():
    assert to_roman(3999) == "MMMCMXCIX"
