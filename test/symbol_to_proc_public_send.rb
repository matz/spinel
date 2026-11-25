# A Symbol's proc, `:m.to_proc` or `s.to_proc`, is a lambda taking a
# receiver and any further arguments, which it sends as public_send does

class Priv
  def pub = :pub
  protected def pro = :pro
  private def pri = :pri
end
p :pub.to_proc.call(Priv.new)
begin; :pro.to_proc.call(Priv.new); rescue NoMethodError => e; p e.message; end
begin; :pri.to_proc.call(Priv.new); rescue NoMethodError => e; p e.message; end
s = :pri
begin; s.to_proc.call(Priv.new); rescue NoMethodError => e; p e.message; end
begin; [Priv.new].map(&s); rescue NoMethodError => e; p e.message; end
pr = :pro.to_proc
begin; [Priv.new].map(&pr); rescue NoMethodError => e; p e.message; end

# further arguments reach the method
ap = :concat.to_proc
g = +"g"
ap.call(g, "!", "?")
p g
up = :upcase.to_proc
p up.call("ab"), up.call("ab", :ascii), up.call(:sy)
pl = :+.to_proc
p pl.call(1, 2), pl.call("a", "b")

# what the lambda reports
p [up.lambda?, up.arity, up.parameters]
sv = :+
pb = sv.to_proc
p [pb.lambda?, pb.arity, pb.parameters]

# errors
begin; up.call; rescue ArgumentError => e; p e.message; end
begin; pl.call(1); rescue ArgumentError => e; p e.message; end
begin; :upcase!.to_proc.call("a".freeze); rescue FrozenError => e; p e.class; end
begin; ap.call(nil, "x"); rescue NoMethodError => e; p e.message; end

# composed, stored, and through levels
rev = :reverse.to_proc
p (up >> rev).call("ab"), (up << rev).call("ab")
H = { up: :upcase.to_proc, cat: :concat.to_proc }
def l3(pr, *a) = pr.call(*a)
def l2(pr, *a) = l3(pr, *a)
def l1(pr, *a) = l2(pr, *a)
p l1(H[:up], "ab")
t = +"a"
l1(H[:cat], t, "b", "c")
p t
class K
  def initialize = @pr = :reverse.to_proc
  def run(v) = @pr.call(v)
end
p K.new.run([1, 2]), K.new.run("ab")
p [H[:up].call("x"), H[:up].call("y")]

# a subclass, and boxed receivers
class MyS < String; end
m = MyS.new("a")
r = ap.call(m, "b")
p r, r.class, r.equal?(m)
[+"s", [1]].each { |v| p ap.call(v, v.is_a?(String) ? "!" : [2]) }
p [:sym, "str"].map { |v| up.call(v) }

# an Enumerable method's name
p :tally.to_proc.call([1, 1, 2])
sm = :sum.to_proc
p sm.call([1, 2]), sm.call([1, 2], 10)
mx = :max.to_proc
p mx.call([3, 1]), mx.call([3, 1, 2], 2)
ts = :tally
p ts.to_proc.call([1, 1, 2])

# a name the builtins answer per call site, and method_missing
p :between?.to_proc.call(5, 1, 9), :clamp.to_proc.call(12, 1, 9)
class Ghost
  def method_missing(n, *a) = [n, a]
  def respond_to_missing?(*) = true
end
p :foo.to_proc.call(Ghost.new), :foo.to_proc.call(Ghost.new, 1, 2)

# passed as a block, the proc `&:m` passes
p [[1], [1, 2]].map(&:size.to_proc), %w[a b].each_with_index.map(&:*.to_proc)
begin; [Priv.new].map(&:pri.to_proc); rescue NoMethodError => e; p e.message; end
p [1, 2].each_slice(1).map(&:first.to_proc), [3, 1, 2].max(2, &:<=>.to_proc)

# a block the proc is called with, which these methods do not take
p pl.call(1, 2) { }, up.call("ab") { }

# a boxed receiver's merge, the proc's operands its splat
mg = :merge.to_proc
w = [{ a: 1 }, 0][0]
p mg.call(w, { b: 2 }), mg.call(w), mg.call(w, { b: 2 }, { c: 3 })
begin; mg.call([nil, 0][1], { b: 2 }); rescue NoMethodError => e; p e.message; end

# an operator's proc passed as a block, as a stored one is
p [3, 1, 2].sort(&:+.to_proc), [3, 1, 2].sort(&(:<=>.to_proc)), [1, 2, 3].inject(&:==.to_proc)
p [3, 1, 2].max(2, &(:<=>.to_proc)), [1, 2, 3].inject(&:<=>.to_proc)
begin; [1, 2].map(&:<.to_proc); rescue ArgumentError => e; p e.class; end

# a method define_method makes of a Symbol's proc takes its receiver first
class Q
  define_method(:sz, :size.to_proc)
  pk = :+.to_proc
  define_method(:pk, pk)
end
p Q.new.sz([1, 2, 3]), Q.new.pk(1, 2)
begin; Q.new.sz; rescue ArgumentError => e; p e.message; end
begin; Q.new.sz(*[]); rescue ArgumentError => e; p e.message; end

# an operator, =~ and the equalities ignore a block, and an empty `**`
# passes nothing
p :===.to_proc.call(Integer, 1) { }
p :=~.to_proc.call("ab", /b/) { }, :eql?.to_proc.call(1, 1) { }, :equal?.to_proc.call(1, 2) { }
begin; pl.call(1) { }; rescue ArgumentError => e; p e.message; end
class Kw; def m(a, k: 0) = [a, k]; end
km = :m.to_proc
h = {}
p km.call(Kw.new, 1, **h)
