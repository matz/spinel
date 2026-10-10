# A parameter whose method raises on its nil at the first thing it does
# (`(a + b) % P`) keeps a plain slot: a caller passing a value that can be
# nil runs every argument, then raises the callee's own error in the
# callee's order. Each nil position, the error class and message, the
# arguments' order with side effects, and a callee whose rescue must still
# catch it (not strict). Methods reached through send, an alias or a
# redefinition are not strict either.
module F
  P = 97
  def self.add(a, b) = (a + b) % P
  def self.sub(a, b) = (a + P - b) % P
  def self.rev(a, b) = (b * a) % P
  def self.cmp(a, b) = a < b
  def self.guarded(a, b)
    (a + b) % P
  rescue NoMethodError, TypeError => e
    "rescued #{e.class}"
  end
end

def show
  r = yield
  puts r.inspect
rescue => e
  puts "#{e.class}: #{e.message}"
end

$log = []
def t(v) = ($log << v; v)

xs = [3, 4]
ys = [5]
n = xs[ARGV.size + 5]
show { F.add(xs[0], xs[1]) }
show { F.add(n, 1) }
show { F.add(1, n) }
show { F.add(n, n) }
show { F.sub(1, n) }
show { F.rev(n, 2) }
show { F.rev(2, n) }
show { F.cmp(n, 1) }
show { F.add(ys[1], xs[0]) }
show { F.add(t(n), t(xs[2])) }
p $log
show { F.guarded(n, 1) }
show { F.guarded(1, n) }
show { F.guarded(1, 2) }
module G
  def self.via_send(a, b) = a + b
  def self.orig(a, b) = a - b
  def self.redef(a, b) = a * b
  def self.redef(a, b) = a * b + 1
  class << self
    alias_method :aliased, :orig
  end
end
show { G.send(:via_send, n, 1) }
show { G.via_send(n, 1) }
show { G.orig(n, 1) }
show { G.aliased(n, 1) }
show { G.redef(2, n) }
show { G.redef(2, 3) }
# a literal nil, and an instance method's own call with an element of an
# ivar table that can be nil
show { F.add(nil, 1) }
show { F.add(2, nil) }
class Banks
  def initialize = @banks = [[1, 2], [3]]
  def take(x) = x + 1
  def pass(i) = take(@banks[i][1])
end
show { Banks.new.pass(0) }
show { Banks.new.pass(1) }
