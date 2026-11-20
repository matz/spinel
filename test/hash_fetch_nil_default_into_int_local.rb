# Hash#fetch answers its default, or its block's value, for a missing key:
# a nil there is the nil of the Integer local it is written to (#12223).
h = {"a" => 1}
x = h.fetch("b", nil)
p x
y = h.fetch("a", nil)
p y
z = h.fetch("b") { nil }
p z
w = h.fetch("b") { |k| k.size }
p w
f = {"a" => 1.5}
v = f.fetch("b", nil)
p v
s = {a: 1}
u = s.fetch(:b, nil)
p u
u = s.fetch(:a, nil)
p u + 1
