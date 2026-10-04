# A builtin iterator on a boxed receiver binds a block's rest, optionals
# and posts, and spreads an Array element across plain requireds, as
# CRuby's block does. `c = c.filter! { }` boxes c (filter! may answer nil),
# and so does a Hash value, an `x || y` or a receiver of two kinds. The
# in-place filters bound only their first name there, so `|*qs|` read nil
# and `|a, b|` took the whole element; map!, each_index, fill, each_slice
# and each_cons left the block's shape unlowered, with the same result.

def filters(c)
  c = c.filter! { |*qs| p qs; qs[0] > 1.6 }
  c = c.reject! { |*qs| p qs; qs[0] > 3 } || c
  c.select! { |q, *r| p [q, r]; true }
  c.keep_if { |q, r = 9| p [q, r]; true }
  c.delete_if { |*qs, z| p [qs, z]; false }
  p c
end

filters([3, 1, 2])
filters([3.5, 1.5, 2.5])

class Holder
  def initialize
    @a = [3.5, 1.5]
    @h = { k: [4, 2] }
  end

  def run
    c = @a
    c = c.filter! { |*qs| q = qs[0]; q > 1 }
    p c
    c = @h[:k]
    c.each_slice(1) { |*qs| p qs }
    c.each_cons(2) { |q, *r| p [q, r] }
    c.each_index { |*qs| p qs }
    c.fill { |*qs| qs[0] * 10 }
    p c
  end
end
Holder.new.run

def pick(n) = n > 0 ? { a: 1, b: 2 } : [[1, 2], [3, 4]]

[1, 0].each do |n|
  c = pick(n)
  c.select! { |a, b| p [a, b]; true }
  c.reject! { |*kv| p kv; false }
  c.filter! { |a, *b| p [a, b]; true }
  p c
end

def maps(c)
  c.map! { |a, b| p [a, b]; b }
  c.collect! { |*qs| p qs; qs.size }
  p c
end
maps([[1, 2], [3, 4]])
maps(nil) rescue p $!.class

def via(x) = yield(x)
via([[5.5, 6.5]] || nil) do |c|
  c.select! { |a, b| p [a, b]; true }
  c.map! { |*qs| p qs; qs[0] }
end
