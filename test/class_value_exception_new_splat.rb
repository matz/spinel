# k.new(*args) with k an exception class in a variable: the spread array's
# first element is the message, as Exception#initialize(msg = nil) takes it.
class A < StandardError; end
class B < ArgumentError; end
class C < StandardError
  def initialize(x = "c") = super("C:#{x}")
end
class K < KeyError; end
class S < SystemExit; end
class D < StandardError; attr_accessor :z; end
def mk(k, *a) = k.new(*a)
def show; p yield; rescue Exception => e; p [e.class, e.message]; end
[A, B, C, D, ArgumentError, RuntimeError].each do |k|
  show { e = mk(k); [e.class, e.message] }
  show { e = mk(k, "q"); [e.class, e.message] }
  show { e = mk(k, nil); [e.class, e.message] }
  show { e = mk(k, 5); [e.class, e.message] }
  show { e = mk(k, "a", "b"); [e.class, e.message] }
end
show { e = mk(K, "kk"); [e.class, e.message] }
show { e = mk(S, 3); [e.class, e.status] }
show { e = mk(S); [e.class, e.status] }
show { e = mk(Errno::ENOENT, "f"); [e.class, e.message] }
args = ["w"]
k = [A, String].sample
show { k.new(*args).class.ancestors.include?(Comparable) || true }
d = mk(D, "dd"); d.z = 1; p d.z, d.message
