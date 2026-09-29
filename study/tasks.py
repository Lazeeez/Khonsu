"""Eight small programming tasks with hidden tests (the agent sees only the task text)."""

TASKS = {
    "fizzbuzz": ("Write a function fizzbuzz(n) in solution.py returning a list of strings for 1..n: "
                 "'Fizz' for multiples of 3, 'Buzz' for 5, 'FizzBuzz' for both, else the number as a string.",
                 "from solution import fizzbuzz\n"
                 "def test_small():\n    assert fizzbuzz(5) == ['1','2','Fizz','4','Buzz']\n"
                 "def test_15():\n    assert fizzbuzz(15)[-1] == 'FizzBuzz'\n"),
    "palindrome": ("Write is_palindrome(s) in solution.py: True if s reads the same backwards ignoring case, "
                   "spaces and punctuation.",
                   "from solution import is_palindrome\n"
                   "def test_basic():\n    assert is_palindrome('A man, a plan, a canal: Panama')\n"
                   "def test_no():\n    assert not is_palindrome('moon god')\n"),
    "roman": ("Write to_roman(n) in solution.py converting 1 <= n <= 3999 to a Roman numeral string.",
              "from solution import to_roman\n"
              "def test_values():\n    assert to_roman(1994) == 'MCMXCIV'\n    assert to_roman(4) == 'IV'\n    assert to_roman(3999) == 'MMMCMXCIX'\n"),
    "anagrams": ("Write group_anagrams(words) in solution.py returning a list of groups (lists) of words that are "
                 "anagrams of each other; groups sorted by first appearance, words in input order.",
                 "from solution import group_anagrams\n"
                 "def test_groups():\n    assert group_anagrams(['eat','tea','tan','ate','nat','bat']) == [['eat','tea','ate'],['tan','nat'],['bat']]\n"),
    "flatten": ("Write flatten(xs) in solution.py that flattens arbitrarily nested lists into a flat list.",
                "from solution import flatten\n"
                "def test_nested():\n    assert flatten([1,[2,[3,[4]],5],[]]) == [1,2,3,4,5]\n"),
    "wordcount": ("Write top_words(text, k) in solution.py returning the k most frequent lowercase words as "
                  "(word, count) tuples, ties broken alphabetically. Words are runs of letters a-z.",
                  "from solution import top_words\n"
                  "def test_top():\n    assert top_words('the moon the sun The moon a', 2) == [('the', 3), ('moon', 2)]\n"),
    "brackets": ("Write balanced(s) in solution.py: True if every (, [, { is closed in the right order.",
                 "from solution import balanced\n"
                 "def test_ok():\n    assert balanced('{[()()]}')\n"
                 "def test_bad():\n    assert not balanced('([)]')\n    assert not balanced('((')\n"),
    "intervals": ("Write merge_intervals(xs) in solution.py merging overlapping [start, end] intervals; "
                  "return them sorted by start.",
                  "from solution import merge_intervals\n"
                  "def test_merge():\n    assert merge_intervals([[1,3],[8,10],[2,6],[15,18]]) == [[1,6],[8,10],[15,18]]\n"),
}
