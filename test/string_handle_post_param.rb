# A String a call hands to a post-required parameter of a block, a proc or
# a lambda (`|*q, t|`, `|n = 0, t|`) is the caller's String (#6179): a
# spliced yield or instance_exec binds it as an alias of the variable, or
# hands it the shared handle, and a dynamic call that the count lands on a
# post pulls the variable into the handle, as for a required parameter.
# Each append is 100 bytes, so a copy cannot pass by capacity.
X = "x" * 100
sh = ->(u) { u << "P" }
def r(a) = yield(1, a)
def r1(a) = yield(a)
def rr(f) = (z = +"z"; f.call(z); yield(1, z); z.size)
def r6(x) = (y = x; yield 1, y)
s = +"a"; r(s) { |*q, t| t << X }; p s.size
s = +"a"; r(s) { |n = 0, t| t << X }; p s.size
s = +"a"; r(s) { |n, *q, t| t << X }; p s.size
s = +"a"; r1(s) { |*q, t| t << X }; p s.size
s = +"a"; sh.call(s); r(s) { |*q, t| t << X }; p s.size
p rr(sh) { |n = 0, t| t << X }
s = +"a"; r6(s) { |*q, t| t << X }; p s.size
p r(nil) { |*q, t| t }
s = +"a"; Object.new.instance_exec(1, s) { |*q, t| t << X }; p s.size
s = +"a"; sh.call(s); Object.new.instance_exec(1, s) { |*q, t| t << X }; p s.size
pr = proc { |*q, t| t << X }
s = +"a"; pr.call(1, s); p s.size
la = ->(n, *q, t) { t << X }
s = +"a"; la.call(1, s); la.(2, 3, s); p s.size
po = proc { |n = 0, t| t << X }
s = +"a"; po.call(s); po[1, s]; p s.size
class K
  def keep(&b) = (@b = b; self)
  def run(x) = @b.call(0, x)
  def kept_yield(x, &b) = (@b = b; yield 1, x)
end
k = K.new.keep { |*q, t| t << X }
s = +"a"; k.run(s); p s.size
s = +"a"; K.new.kept_yield(s) { |*q, t| t << X }; p s.size
fs = [proc { |*q, t| t << X }, 1][ARGV.size]
s = +"a"; fs.call(0, s); p s.size
rd = proc { |*q, t| t.size }
s = +"a"; p rd.call(1, s), s.size
fz = "fr".freeze
begin; r(fz) { |*q, t| t << X }; rescue FrozenError => e; p e.class; end
begin; pr.call(1, fz); rescue FrozenError => e; p e.class; end
# a block yielding its own parameter on, to a caller's block whose post the
# yield's count lands it on
def ry1(x) = [x].each { |u| yield u }
s = +"a"; ry1(s) { |a = 0, t| t << X }; p s.size
def ry2(x) = [x].each { |u| yield 9, u }
s = +"a"; ry2(s) { |a, *q, t| t << X }; p s.size
