# A program's Object method of a Kernel name (tap, then, yield_self, itself,
# frozen?, dup) answers every receiver the lookup reaches Object for: a
# literal, a constant, a local, a program object, a Struct, a builtin
# subclass, a tap in statement position, a Time, a File. The builtin answers
# a call that runs before the def (one in a block too), and a class value's
# own singleton methods (File.basename) come first.
K = 7
S = Struct.new(:a)
class Foo; end
class MyA < Array; end

def show = yield
p 1.frozen?, "s".frozen?, [1].frozen?
p show { 1.frozen? }
[2].tap { |a| puts "before #{a.size}" }

class Object
  def tap
    puts "my tap"
    yield self
    :tap
  end
  def then = [:then, yield(self).class]
  def yield_self = [:ys, yield(self).class]
  def itself = :me
  def frozen? = :mine
  def dup = :dup
  def basename(*a) = :mine
end

p 1.tap { |x| puts "i#{x}" }
p "s".tap { |x| puts "s#{x}" }
p [1].tap { |x| puts "a#{x.size}" }
p({a: 1}.tap { |x| puts "h#{x.size}" })
p (1..2).tap { |x| puts "r#{x.first}" }
p nil.tap { |x| puts "n#{x.inspect}" }
p K.tap { |x| puts "k#{x}" }
x = "five"
p x.tap { |v| puts "x#{v}" }
p S.new(1).tap { |v| puts "s#{v.a}" }
p Foo.new.tap { |v| puts v.class }
p MyA.new.then { |v| v.class }
p 1.then { |v| v + 1 }
p [1, 2].then { |v| v.sum }
p [1, 2].yield_self { |v| v.size }
p 1.itself, "s".itself, [1].itself, :s.itself, nil.itself, 2.5.itself, K.itself
p 1.frozen?, "s".frozen?, [1].frozen?, :s.frozen?, nil.frozen?, Foo.new.frozen?
p 1.dup, "s".dup, [1].dup, Foo.new.dup
[3].tap { |a| puts "stmt #{a.size}" }
"t".tap { |s| puts "stmt #{s}" }
5.tap { |i| puts "stmt #{i}" }
t = Time.at(0)
p t.then { |v| v.year }
p t.itself.class
p $stdout.then { |o| o.class }
p File.basename("a/b")
