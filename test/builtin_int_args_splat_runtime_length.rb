# A splat whose length only the run time knows, into a builtin that takes
# integer arguments: each length reaches the form of that many arguments
# (slice(*[0, 2]) is slice(0, 2), not slice(0)), and a length the builtin
# has no form for raises CRuby's ArgumentError.
IDX = [0, 2]
ONE = [1]
E = []
O = [2]
T = [1, 2]
R = 1..2
N = nil
KEYS = [:a, :c, :d]
def idx(n) = n == 1 ? [1] : n == 2 ? [1, 2] : n == 0 ? [] : [0, 1, 2]
def e = []
def o = [2]
def ks = [:b, :d]

a = [10, 20, 30, 40]
s = "hello"
p a.slice(*IDX), a[*IDX], a.slice(*ONE), a[*ONE]
p s.slice(*IDX), s[*IDX], s.slice(*ONE), s[*ONE]
p s.byteslice(*IDX), s.byteslice(*ONE)
[1, 2, 0, 3].each do |n|
  begin; p a.slice(*idx(n)); rescue ArgumentError => x; p x.message; end
  begin; p a[*idx(n)]; rescue ArgumentError => x; p x.message; end
  begin; p s.slice(*idx(n)); rescue ArgumentError => x; p x.message; end
  begin; p s[*idx(n)]; rescue ArgumentError => x; p x.message; end
  begin; p s.byteslice(*idx(n)); rescue ArgumentError => x; p x.message; end
end
p a.slice(*R)
begin; p a.slice(*N); rescue ArgumentError => x; p x.message; end

def f(arr, *r) = arr.slice(*r)
def g(str, *r) = str[*r]
p f(a, 1), f(a, 1, 2), g(s, 1), g(s, 1, 3)

h = {a: 1, b: 2, c: 3, d: 4}
p h.slice(*KEYS), h.slice(*[:a]), h.slice(*ks)

class Foo
  def slice(*x) = x.size
end
p Foo.new.slice(*idx(3))

p a.values_at(*IDX), a.values_at(*e), a.fetch(*ONE), [10, 20, 30].fetch(*[5, :d].dup)
p a.first(*ONE), a.last(*ONE), a.rotate(*ONE), a.rotate(*E), a.first(*e), a.last(*o)
p a.sample(*E).class, a.sample(*O).size
p a.max(*E), a.max(*O), a.min(*e), a.min(*o)
b = [1, 2, 3, 4]
p b.pop(*E), b.pop(*O), b.shift(*e), b
c = [1, 2, 3]
c.insert(*[1, 99].dup)
p c

p s.center(*[9]), s.ljust(*[9]), s.rjust(*o), s.center(*[9, "*"].dup), s.ljust(*[8, "*"].dup)
p 1234.digits(*[10]), 1234.digits(*[100].dup), 1234.digits(*e)
p 3.pow(*[4, 5]), 3.pow(*[4]), 2.pow(*T), 2.pow(*o)
p 12.345.round(*E), 12.345.round(*O), 12.345.floor(*o), 12.345.ceil(*o), 12.345.truncate(*e)
p 1234.round(*[-2])

begin; [1].fetch(*e); rescue ArgumentError => x; p x.message; end
begin; [1].first(*[1, 2].dup); rescue ArgumentError => x; p x.message; end
begin; 2.pow(*[1, 2, 3].dup); rescue ArgumentError => x; p x.message; end
