# A class_eval / class_exec / module_eval block runs with the receiving
# class as self. A bare call in its body that the class answers (its own
# class method, an inherited one, or Module's name / to_s / inspect) runs
# on that class, not on the self of the code around the block.
class Foo
  def self.bar = "Foo.bar"
  def self.add(a, b) = a + b
  def self.make = new
end
class Sub < Foo; end
class A
  def self.bar = "A.bar"
  def self.k = Foo.class_eval { name }
  def self.j = Foo.class_eval { bar }
  def self.s = Sub.class_eval { [name, to_s, inspect, bar, add(1, 2)] }
  def self.t(x) = Foo.class_exec(x) { |v| add(v, 10) }
  def self.u = Foo.class_eval { puts "in #{name}"; 7 }
end
p A.k
p A.j
p A.s
p A.t(5)
p A.u
p Foo.class_eval { name }
p Foo.module_eval { bar }
p Sub.class_eval { make.class }

# Private and protected class methods: a bare call reaches them.
class Pv
  def self.pv = "Pv.pv"
  private_class_method :pv
  class << self
    private def sp = "Pv.sp"
    protected def pt = "Pv.pt"
  end
end
class SubPv < Foo
  private_class_method :bar
end
class B
  def self.pv = "B.pv"
  def self.sp = "B.sp"
  def self.pt = "B.pt"
  def self.bar = "B.bar"
  def self.k = Pv.class_eval { [pv, sp, pt] }
  def self.m = SubPv.class_eval { bar }
end
p B.k
p B.m
p((Pv.pv rescue $!.class))
