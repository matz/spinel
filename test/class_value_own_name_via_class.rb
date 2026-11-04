# x.class.name / to_s / inspect calls the class's own class method of that
# name, as C.name does; the other classes answer Module#name.
class C
  def self.name = "NM"
  def who = self.class.name
end
class D < C; end
class E; end
class F; def self.to_s = "FF"; def self.inspect = "FI"; end
S = Struct.new(:a)
p C.new.class.name, D.new.class.name, E.new.class.name
p C.new.who, D.new.who
p 3.class.name, "s".class.name, nil.class.name, [].class.name
p S.new(1).class.name
p F.new.class.to_s, F.new.class.inspect, F.new.class.name, E.new.class.to_s
xs = [C.new, E.new, 1, "x", F.new]
xs.each { |x| p [x.class.name, x.class.to_s] }
def nm(o) = o.class.name
p nm(C.new), nm(E.new), nm(D.new)
module M; def self.name = "MM"; end
class G; include M; end
p G.new.class.name
k = Class.new
p k.new.class.name

# a builtin value's class answers its own name beside the program's methods
begin; raise ArgumentError; rescue => e; p e.class.name, e.class.to_s; end
p ("a".."b").class.name, (1.0..2.0).class.name, Random.new.class.name
e2 = (ARGV.size > 5 ? RuntimeError.new : nil)
p e2.class.name
