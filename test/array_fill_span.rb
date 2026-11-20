# fill on an Integer or Float Array stores its span in one run
# (sp_<K>Array_fill_span): the slots, a growth past the end with a nil gap,
# a negative start, a Range, a nil start or length, a frozen receiver, an
# Array with nils, and one whose front was shifted off. Each fill is printed
# before the next runs, so each one's own effect is checked.
a = [1, 2, 3, 4, 5]
p a.fill(0)
p a.fill(7, 2)
p a.fill(9, 1, 2)
p a.fill(4, 3, 4)
b = [1, 2]; p b.fill(5, 4, 2)
c = [1, 2, 3]; p c.fill(8, -2)
begin; c.fill(6, -9, 1); rescue => x; p x.class; end
d = [1, 2, 3]
p d.fill(0, 1, 0)
p d.fill(0, 1, -3)
p d.fill(0, 5)
e = [1, nil, 3, nil]
p e.fill(2, 1, 2)
p e.fill(0)
f = [1, 2, 3, 4]; f.shift
p f.fill(0, 1)
p f.fill(6, 2, 3)
g = [1, 2, 3].freeze
begin; g.fill(0); rescue => x; p x.class; end
h = [1, 2, 3]
p h.fill(0, 1..)
p h.fill(5, 0...1)
p h.fill(6, -2..-1)
k = []
p k.fill(3)
p k.fill(3, 0, 3)
m = [1, 2, 3]; s = nil
p m.fill(4, s)
p m.fill(1, 1, nil)

fa = [1.5, 2.5, 3.5]
p fa.fill(0.0)
p fa.fill(-0.0, 1)
p fa.fill(1e300, 2, 3)
fb = [1.0]; p fb.fill(2.5, 3, 2)
fe = [1.0, nil, 3.0]; p fe.fill(2.0, 1, 1)
ff = [1.0, 2.0].freeze
begin; ff.fill(0.0); rescue => x; p x.class; end

def clear(a, k) = a.fill(0, 0, k + 1)
q = Array.new(6) { |i| i }
p clear(q, 2)
p clear(q, 7)
