# respond_to? and defined? of a class method a method body defines answer
# false (nil) before that body runs and true after, as the call they guard
# meets the method only once it exists

class A; end
def set_a = (def A.k = :ka)
p A.respond_to?(:k), defined?(A.k)
def go_a = (A.respond_to?(:k) ? A.k : :none)
def df_a = defined?(A.k)
p go_a, df_a
set_a
p A.respond_to?(:k), defined?(A.k)
p go_a, df_a

# a guard that returns early, and a subclass's lookup
class B; end
class B2 < B; end
def set_b = (def B.k = :kb)
def go_b
  return :none unless B2.respond_to?(:k)
  B2.k
end
p go_b
set_b
p go_b

# the class's earlier class method, and an instance method of the name
class C
  def self.j = :j
  def k = :inst
end
def set_c = (def C.k = :kc)
def go_c = [C.respond_to?(:j), C.respond_to?(:k), C.new.respond_to?(:k), C.new.k]
p go_c
set_c
p go_c

# a builtin method of the name answers true before and after
class D; end
def set_d = (def D.name = "named")
def go_d = [D.respond_to?(:name), D.name]
p go_d
set_d
p go_d

# two classes, each with its own body
class E; end
class F; end
def set_e = (def E.k = :ke)
def set_f = (def F.k = :kf)
def go_ef = [E.respond_to?(:k), F.respond_to?(:k)]
p go_ef
set_f
p go_ef
set_e
p go_ef

# a body run inside a block, and one never called
class G; end
def set_g = (def G.k = :kg)
r = []
2.times { |i| r << G.respond_to?(:k); set_g if i == 0 }
p r
class H; end
def set_h = (def H.k = :kh)
p H.respond_to?(:k), defined?(H.k)
set_h if r.size > 5
p H.respond_to?(:k)
