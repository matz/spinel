# raise X, m, bt: X.exception(m) makes the exception (a class's own
# self.exception too), set_backtrace(bt) attaches bt, and the result is
# raised. A class value with a cause: keyword calls its own
# self.exception as well.
class A < StandardError
  def self.exception(*a) = new("A-ex:#{a.first}")
end
class B < StandardError; end
def show
  yield
rescue Exception => e
  puts "#{e.class}: #{e.message} #{e.backtrace.inspect} #{e.cause.inspect}"
end
$n = 0
def msg = ($n += 1; "m#{$n}")
def bts = ($n += 10; ["b#{$n}"])
show { raise B, "m", ["bt1"] }
show { fail B, "m", ["f1", "f2"] }
show { raise A, msg, bts }
show { raise RuntimeError, "r", ["y"] }
show { raise ArgumentError, "r", [] }
k = B
show { raise k, "m", ["k1"] }
ks = [A, B]
ks.each { |kk| show { raise kk, "m", ["z"] } }
show { raise ks[0], "m", ["z"], cause: B.new("c") }
show { raise A, "m", ["w"], cause: nil }
show { begin; raise "in"; rescue; raise B, "m", ["q"]; end }
def deep(k) = raise(k, "d", ["deep"])
show { deep(A) }
show { deep(B) }
ka = A
begin; raise ka, "m", cause: nil; rescue => e; puts "#{e.class}: #{e.message}"; end
ks.each { |kk| begin; raise kk, "m", cause: B.new("cc"); rescue => e; puts "#{e.class}: #{e.message} #{e.cause.inspect}"; end }
p $n
