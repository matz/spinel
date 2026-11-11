# A frozen String stored in a Hash reached through a boxed (poly) value keeps
# its identity when the runtime copies the Hash's entries: the runtime
# objects read a frozen handle the way the generated C does, as the String
# itself, not as a frozen copy of its bytes.

s = +"v"
s.freeze
h = {"a" => "x"}
o = [{"a" => s}, 1][0]
h.merge!(o)
puts h["a"].equal?(s)
puts h["a"].frozen?

u = +"u"
u.freeze
h2 = {"a" => "x"}
o2 = [{"a" => u}, 1][0]
h2.update(o2)
puts h2["a"].equal?(u)

w = +"w"
w.freeze
h3 = {"b" => "y"}
o3 = [{"a" => w}, 1][0]
m = h3.merge(o3)
puts m["a"].equal?(w)
puts m["b"]

z = +"z"
z.freeze
o4 = [{"a" => "q"}, 1][0]
o4["a"] = z
puts o4["a"].equal?(z)

k = +"k"
k.freeze
a = [[k], 1][0]
b = ["p"]
b.concat(a)
puts b[1].equal?(k)
