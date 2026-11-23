# An Array's each walk reads its element, and other arrays at a counter it
# steps once per pass (`i = 0` ahead of the walk, `i += 1` last in its body),
# in place behind one test ahead of the walk, as a while over the same array
# does. each_with_index is such a walk (enumerable.rb), its block's index
# bound from that counter at each pass. Every shape keeps CRuby's answers:
# one that writes an array, rebinds the counter or steps it otherwise reads
# as before.
def dot_ewi(a, b) = (s = 0.0; a.each_with_index { |x, i| s += x * b[i] }; s)
def idot_ewi(a, b) = (s = 0; a.each_with_index { |x, i| s += x * b[i] }; s)
def first_ewi(a, t) = (a.each_with_index { |x, i| return i if x > t }; -1)
def dot_count(a, b) = (s = 0.0; i = 0; a.each { |x| s += x * b[i]; i += 1 }; s)
def dot_odd(a, b) = (s = 0.0; a.each_with_index { |x, i| s += x * b[i] }; s)

a = [1.0, 2.0, 3.0, 4.0]; b = [10.0, 20.0, 30.0, 40.0]
p dot_ewi(a, b), dot_ewi(b, a), idot_ewi([1, 2, 3], [4, 5, 6]), first_ewi(a, 2.5), first_ewi(a, 9.0)
p dot_count(a, b), dot_ewi([], b), dot_ewi([1.0], [2.0, 9.0])
# a shorter or nil-holding operand reads as before: nil * Float raises
begin; dot_odd(a, [1.0, 2.0]); rescue TypeError => e; p e.class; end
begin; dot_odd([1.0, nil], b); rescue NoMethodError => e; p e.class; end
begin; dot_odd(a, [1.0, nil, 3.0, 4.0]); rescue TypeError => e; p e.class; end

# the block writes the array it reads, rebinds it, or shrinks the receiver
s = 0.0; a.each_with_index { |x, i| b[i] = x; s += b[i] }; p s, b
b = [10.0, 20.0, 30.0, 40.0]
s = 0.0; a.each_with_index { |x, i| s += x * b[i]; b = [1.0] * 4 if i == 1 }; p s
q = [1.0, 2.0, 3.0]; s = 0.0; q.each_with_index { |x, i| s += x * q[i]; q.pop if i == 0 }; p s, q
# next, break, the index rebound in the block
b = [10.0, 20.0, 30.0, 40.0]
s = 0.0; a.each_with_index { |x, i| next if i == 1; s += x * b[i] }; p s
s = 0.0; a.each_with_index { |x, i| break if i == 2; s += x * b[i] }; p s
s = 0.0; a.each_with_index { |x, i| i = 0; s += x * b[i] }; p s

# counters that are not the pass index, or a next that skips the step
s = 0.0; i = 1; a.each { |x| s += x * (b[i] || 0.0); i += 1 }; p s
s = 0.0; i = 0; a.each { |x| next if x == 2.0; s += x * b[i]; i += 1 }; p s, i
s = 0.0; i = 0; a.each { |x| i += 1; s += x * (b[i] || 0.0) }; p s
begin; s = 0.0; i = 0; a.each { |x| s += x * b[i]; i += 2 }; rescue TypeError => e; p e.class; end
s = 0.0; i = 0; a.each { |x| s += x * b[i]; [1].each { i = 0 }; i += 1 }; p s
s = 0.0; i = 0; f = nil; a.each { |x| f = -> { b[i] }; s += x * b[i]; i += 1 }; p s, f.call
s = 0.0; i = 0; a.each { |x| s += x * b[i]; b = [0.0] * 4; i += 1 }; p s

# a user method's walk yields the counter: bound to the block's parameter
# at its position, or spread after a prefix, which moves it
def walk_pos(a) = (i = 0; a.each { |x| yield x, i; i += 1 })
def walk_spread(pre, a) = (i = 0; a.each { |x| yield(*pre, i); i += 1 })
def sum_pos(a, b) = (s = 0.0; walk_pos(a) { |x, q| s += x * b[q] }; s)
def sum_spread(pre, a, b) = (s = 0.0; walk_spread(pre, a) { |x, q = 0| s += b[q] }; s)
b = [10.0, 20.0, 30.0, 40.0]
p sum_pos(a, b), sum_spread([], a, b), sum_spread([7], a, b), sum_spread([5, 2], a, b)
begin; sum_spread([5, 300000000], a, b); rescue TypeError => e; p e.class; end
