# An append accumulator -- a string local appended to inside a loop -- rides
# the growable handle so each `<<` is amortized O(1) rather than a copy of the
# whole accumulation. The representation change must be invisible: what these
# pin is that the results, the aliasing, and the shapes that must NOT be
# promoted all still behave.
def build(n)
  out = +""
  i = 0
  while i < n
    out << "ab"
    i += 1
  end
  out
end
p build(5).length
p build(5)

# a block-form loop accumulates the same way, and the value survives the method
def joined(list)
  s = +""
  list.each { |x| s << x.to_s }
  s
end
p joined([1, 2, 3])

# read after the loop, several ways
acc = +""
3.times { |i| acc << i.to_s }
p acc
p acc.length
p acc[1]
p acc.upcase
p acc == "012"
p acc + "!"
p [acc].first

# a local READ inside the loop keeps its plain representation and its answers
r = +""
sizes = []
4.times do |i|
  r << "x"
  sizes << r.length
end
p sizes
p r

# a param appended through a call is the caller's String: the caller sees the append
def feed(buf)
  buf << "!"
end
b = +""
2.times { feed(b) }
p b

# a captured local is shared storage, and both writers are visible
w = +""
f = proc { w << "c" }
2.times { w << "a" }
f.call
p w

# `<<` outside any loop is unchanged
z = +""
z << "one"
z << "two"
p z

# an each_with_object memo is the iterator's slot, not ours
p({ "a" => 1, "b" => 2 }.each_with_object(+"") { |(k, _v), a| a << k })
