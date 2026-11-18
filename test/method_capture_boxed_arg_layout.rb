# The boxed argument of method_capture_boxed_arg.rb, into parameters whose
# place in the call is not their index: behind a leading optional, behind a
# rest, and a trailing optional. Each method is typed by direct calls with
# Integers first.
def lead_opt(a = 1, b) = [a, b]
def with_rest(a, *r, z) = [a, r, z]
def trailing(a, b = 2) = [a, b]
p lead_opt(1, 2), lead_opt(3)
p with_rest(1, 2, 3)
p trailing(1), trailing(1, 2)
TABLE = {lo: method(:lead_opt), wr: method(:with_rest), tr: method(:trailing)}
def one(name, x) = TABLE[name].call(x)
def second(name, y) = TABLE[name].call(1, y)
def third(name, z) = TABLE[name].call(1, 2, z)
p one(:lo, 1.5), one(:lo, "s")
p second(:lo, 2.5), second(:lo, "t")
p one(:tr, 1.5), second(:tr, "u"), second(:tr, 2.5)
p second(:wr, 2.5), third(:wr, "z"), third(:wr, 3.5)
p lead_opt(4, 5), with_rest(6, 7), trailing(9)
