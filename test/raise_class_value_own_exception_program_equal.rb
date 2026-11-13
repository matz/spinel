# raise k, m with k a class value calls k's own self.exception even when
# the program defines its own equal? (on Object, BasicObject or the class).
class A < StandardError
  def self.exception(*a) = (a.empty? ? new("A-ex") : new("A-ex:#{a.first}"))
  def equal?(o) = :inst
end
class C < A; def self.exception(*a) = new("C-ex:#{a.first}"); end
class Object; def equal?(o) = nil; end
class BasicObject; def equal?(o) = true; end
def show
  yield
rescue Exception => e
  puts "#{e.class}: #{e.message}"
end
k = A.new("inst")
show { raise k }
show { raise k, "two" }
k = C
show { raise k, "sub" }
k = nil
show { raise k, "nil" }
k = "str"
show { raise k, "s" }
@k = A
show { raise @k, "ivar" }
show { raise [C, 1].first, "call" }
show { raise (1 > 0 ? A : RuntimeError), "tern" }
$g = A
show { raise $g, "gvar" }
def m(kl) = (raise kl, "param")
show { m(A) }
show { m(KeyError) }
show { raise A.new("i2") }
p A.equal?(A), 1.equal?(2)
