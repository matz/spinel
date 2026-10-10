# Integer and Float answer the Numeric methods they inherit (abs2, quo,
# positive?, step, ...) through method(:sym): the name raised NameError, or
# under a Float receiver failed to compile, as the class's own table leaves
# the superclass's out. The Method reports Numeric as its owner and CRuby's
# arity.
m = 5.method(:abs2)
p m.call, m.arity, m.owner, m.name
q = 5.method(:quo)
p q.call(2), q.arity, q.owner
p 5.method(:positive?).call, (-3).method(:negative?).call, 0.method(:nonzero?).call
p 5.method(:real?).call, 5.method(:to_c).call, 5.method(:polar).call
s = 1.method(:step)
p s.arity, s.owner
p 1.method(:abs).owner, 1.method(:abs).arity
f = 2.5.method(:abs2)
p f.call, f.arity, f.owner
p 2.5.method(:div).call(1), 2.5.method(:remainder).call(1), 2.5.method(:integer?).call
begin
  5.method(:quo).call
rescue ArgumentError => e
  p e.message
end
# clone and dup are Numeric's too: clone takes only `freeze:`, arity -1
c = 5.method(:clone)
p c.owner, c.arity, c.call
d = 2.5.method(:dup)
p d.owner, d.arity, d.call
p 5.method(:eql?).owner, 5.method(:eql?).arity, 5.method(:eql?).call(5)
