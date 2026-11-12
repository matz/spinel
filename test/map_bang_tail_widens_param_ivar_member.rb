# A map! or fill block whose last value the Array cannot hold (no next)
# widens the Array when the receiver is a param, an ivar or a Struct member.
def widen(a) = a.map! { |x| x == 2 ? 2.5 : x }
arr = [1, 2, 3]
p widen(arr)
class Box; attr_accessor :a; def initialize = @a = [1, 2, 3]; def go = @a.map! { |x| x > 1 ? x * 0.5 : x }; end
b = Box.new; b.go; p b.a
c = Box.new
c.a.map! { |x| x > 1 ? "s" : x }
p c.a
S = Struct.new(:v)
s = S.new([1, 2])
s.v.map! { |x| x == 1 ? 1.5 : x }
p s.v

def w1(a) = a.map! { |x| (x if x > 2) }
p w1([1, 2, 3])
def w2(a) = a.fill { |i| i == 0 ? :s : i }
p w2([5, 6, 7])
def w3(a) = a.map! { |x| x.to_s }
p w3([1, 2])
def w4(a) = a.map! { |x| x * 2 }
p w4([1, 2])
def w5(a) = a.map! { |x| x == "a" ? 1 : x }
p w5(["a", "b"])
def w6(a) = a.collect! { |x| x == 1.5 ? nil : x }
p w6([1.5, 2.5])
class B2; attr_reader :r; def initialize = @r = [1, 2]; def go = @r.map! { |x| x.even? ? [x] : x }; end
b2 = B2.new; b2.go; p b2.r
def w7(a)
  a.map! { |x| x > 1 ? 2.5 : x }
  a.sum
end
p w7([1, 2, 3])
arr = [1, 2]
def w8(a) = a.map! { |x| x == 1 ? "one" : x }
w8(arr)
p arr
p arr.size

y = [0, 0, 0]
y.fill { |i| i == 1 ? nil : 5 }
p y
def fn(a) = a.fill { |i| (i if i == 1) }
p fn([0, 0])
z = [1, 2]
z.map! { |x| x == 1 ? nil : x }
p z
