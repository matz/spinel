# CRuby's pp lays an Array out by its own pretty_print, so the program's
# own Array#inspect does not run for it, nested in a Hash or another Array
# either; p still runs it
class Array; def inspect = "ARR"; end
pp [1]
pp [[1], 2]
x = pp([3])
p x
pp({a: [1]})
pp 1, [2]
a = [5]
pp a, *[[6]]
def m(v) = pp(v)
q = m([7])
p q
h = {k: [8]}
pp h
p h
p [1]
