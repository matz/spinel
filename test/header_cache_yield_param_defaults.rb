# A loop that yields runs the caller's block at each pass, the block spliced
# in where it yields. The block's parameter defaults (an optional's, a
# keyword's) run at each pass too, before its body: one that clears or pops
# an array the loop reads must leave the loop reading the array as it is
# now, not a header read ahead of the loop. CRuby raises TypeError for each
# (nil * Float once the array is empty).
def walk(a) = (i = 0; a.each { |x| yield x, i; i += 1 })
def upto_n(n) = (i = 0; while i < n; yield i; i += 1; end)
def times_n(n) = n.times { |i| yield i }

def clear_dflt(a, b) = (s = 0.0; walk(a) { |x, q, z = (b.clear; 0)| s += x * b[q] }; s)
def pop_dflt(a, b) = (s = 0.0; walk(a) { |x, q, z = b.pop| s += x * b[q] }; s)
def kw_dflt(a, b) = (s = 0.0; walk(a) { |x, q, k: (b.clear; 0)| s += x * b[q] }; s)
def ewi_dflt(a, b) = (s = 0.0; a.each_with_index { |x, i, z = (b.clear; 0)| s += x * b[i] }; s)
def while_dflt(a, b) = (s = 0.0; upto_n(a.length) { |i, z = (b.clear; 0)| s += a[i] * b[i] }; s)
def times_dflt(a, b) = (s = 0.0; times_n(a.length) { |i, z = (b.clear; 0)| s += a[i] * b[i] }; s)
def rebind_dflt(a, b) = (s = 0.0; walk(a) { |x, q, z = (b = [7.0, 8.0]; 0)| s += x * b[q] }; s)
%i[clear_dflt pop_dflt kw_dflt ewi_dflt while_dflt times_dflt].each do |m|
  begin
    p send(m, [1.0, 2.0, 3.0], [10.0, 20.0, 30.0, 40.0])
  rescue TypeError => e
    p [m, e.class]
  end
end

# defaults that leave the arrays alone, and the other parameter shapes
# (a rest, a post, a block parameter), answer as before
b = [10.0, 20.0, 30.0, 40.0]
s = 0.0; walk([1.0, 2.0]) { |x, q, z = 5| s += x * b[q] + z }; p s
s = 0.0; walk([1.0, 2.0]) { |x, *r| s += x * b[r[0]] }; p s
s = 0.0; walk([1.0, 2.0]) { |x, q, &blk| s += x * b[q] }; p s
s = 0.0; upto_n(3) { |i, k: 2| s += b[i] * k }; p s
p clear_dflt([], [1.0]), while_dflt([], [])
# a default that rebinds the array the loop reads
p rebind_dflt([1.0, 2.0], [10.0, 20.0, 30.0, 40.0])
