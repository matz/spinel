# A typed Array subject in a statement `case` against a splatted list of
# Integers, Strings or Floats: no element can match, the list still runs
# once, and a later condition is still tested.
$s = 0
def subj; $s += 1; [1, 2]; end
$l = 0
def ints; $l += 1; [1, 2, 3]; end
case subj when *ints then p :s_ints else p :s_no end
p(case subj when *ints then :v_ints else :v_no end)
strs = ["a", "b"]
case subj when *strs then p :s_strs else p :s_no end
floats = [1.5, 2.0]
case subj when *floats then p :s_floats else p :s_no end
case subj when *ints, [1, 2] then p :s_later else p :s_no end
p [$s, $l]
# any other subject of another family than the list's elements: a String,
# a Symbol, true or false
case "a" when *ints then p :str_ints else p :str_no end
case :a when *ints then p :sym_ints else p :sym_no end
case :a when *strs then p :sym_strs else p :sym_no end
tt = 1 < 2
case tt when *[1] then p :true_hit else p :true_no end
ff = 1 > 2
case ff when *[0.0] then p :false_hit else p :false_no end
