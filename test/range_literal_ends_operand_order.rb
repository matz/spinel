# A Range literal's ends, and variable operator writes (`n += 1`, `a ||= 1`),
# run in the order written when they are a call's operands.
x = [1, 2, 3, 4, 5]
s = "abcdef"
n = 0
p x[(n += 1)..(n += 1)]
m = 2
p x[m..(m = 3)]
a = nil
p x[(a = 1)..a]
q = 0
p s[(q += 1)..(q += 2)]
n = 0
p x.slice((n += 1)...(n += 2))
n = 0
p x.values_at((n += 1)..(n += 1))
n = 0
p 5.clamp((n += 1)..(n += 3))
n = 0
p x.dup.fill(0, (n += 1)..(n += 1))
n = 0
p (1..10).step(2).to_a[(n += 1)..(n += 1)]
n = 0
p x.each_slice(2).to_a[(n += 1)..(n += 1)]
n = 0
p "hello".include?(s[(n += 1)..(n += 1)])
n = 0
p s[n += 1, n += 1]
@n = 0
p s[@n += 1, @n += 1]
$g = 0
p s[$g += 1, $g += 1]
@c = nil
p s[@c ||= 1, @c += 1]
b = 1
p x.fetch(b &&= 2, b += 1)
class K
  @@v = 0
  def self.t(s) = s[@@v += 1, @@v += 1]
end
p K.t(s)
def mm(s)
  i = 0
  s.byteslice(i += 1, i += 2)
end
p mm(s)
n = 0
p x.values_at(n += 1, (n += 1)..(n += 1))
n = 0
p [s.index("c", n += 1), n]
