# eql? and equal? that no program method answers are identity; the
# receiver runs, and first, even where the answer does not need it: an
# argument whose box holds no object, a value-typed object, a Fiber
$log = []
def arg(v) = ($log << :arg; v)

# a value-typed object (no identity is read from it)
class V
  def initialize(a = 1) = @a = a
end
def mkv = ($log << :v; V.new)
s = [:s, 1].first
p mkv.equal?(s), mkv.eql?(arg(2)), $log
$log.clear

# an object compared with a boxed argument through eql?
class K
  def initialize(a = 1) = @a = a
end
def mkk = ($log << :k; K.new)
k = K.new
p k.eql?(k), mkk.eql?(s), mkk.eql?(arg(nil)), $log
$log.clear
p [1, 2].map { mkk.eql?(arg(:a)) }, $log.size
def tail = mkk.eql?(arg(2))
p tail, mkk.equal?(s) || mkk.eql?(arg(:q)), $log
$log.clear
def maybe(f) = f ? mkk : nil
p [maybe(true), maybe(false)].map { |o| o.eql?(arg(1)) }, $log
$log.clear

# a Fiber
def fib = ($log << :f; Fiber.new { 1 })
f = fib
p fib.equal?(s), fib.eql?(arg(3)), f.equal?(f), $log
