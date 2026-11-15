# map!/collect!.with_index whose block gives a value of another kind
# widens the Array, as a plain map! block does.
a = [1, 2]
a.map!.with_index { |x, i| x * 10 + i }
p a
b = [1, 2]
b.map!.with_index { |x, i| i == 0 ? :w : x }
p b
c = [1, 2]
c.map!.with_index { |x, i| i == 0 ? "s" : x }
p c
e = [1, 2]
e.map!.with_index(1) { |x, i| i == 1 ? 1.5 : x }
p e
f = [1, 2]
f.collect!.with_index { |x, i| i == 0 ? nil : x }
p f
p f.map(&:class)

g = %w[a b]
g.map!.with_index { |x, i| i == 0 ? 7 : x }
p g

# a nil arm on a typed ivar keeps the String Array (it holds nil)
class Names
  def initialize; @a = ["a", "b"]; end
  def blank_first
    @a.map!.with_index { |x, i| i == 0 ? nil : x }
    @a
  end
end
p Names.new.blank_first

# the block given as a proc, on a local that owns its Array
s = ["a", "b"]
s.map!.with_index(&proc { |x, i| x * (i + 1) })
p s
pr = proc { |x, i| i == 0 ? :z : x }
u = [1, 2]
u.map!.with_index(&pr)
p u
