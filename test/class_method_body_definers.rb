# A class method's define_method, define_singleton_method and attr_* calls
# define their methods when the class method runs: calls, respond_to?,
# method_defined?, send, a poly receiver, a subclass and a nil-able receiver
# read each before and after it.

# attr_reader, attr_writer and attr_accessor in one body
class Box
  def initialize = (@a = 1; @b = 2; @c = 3)
  def self.open_up = (attr_reader :a; attr_writer :b; attr_accessor :c; :ok)
end
k = Box.new
p [(k.a rescue $!.class), k.respond_to?(:c), Box.method_defined?(:b=)]
p Box.open_up
p k.a, (k.b = 9), k.c, (k.c += 1), k.c, k.send(:a), [k, nil].map { |x| x&.a }
p Box.instance_methods(false).sort

# an accessor op-assigned through, and on a nil-able receiver
class Cell
  def initialize = (@m = 1)
  def self.setup = attr_accessor(:m)
end
c = Cell.new
p(begin
  c.m += 1
rescue NoMethodError => e
  e.class
end)
Cell.setup
c.m += 1
c.m ||= 9
p c.m
o = ARGV.size > 99 ? nil : Cell.new
o&.m = 40
p o&.m, [Cell.new, c].map(&:m).sum

# define_method on a poly receiver and a nil-able slot
class Shape
  def self.install = define_method(:area) { :shape }
end
class Plain
  def area = :plain
end
xs = [Shape.new, Plain.new]
p xs.map { |x| (x.area rescue $!.class) }
Shape.install
p xs.map(&:area)
y = [nil, Shape.new][1]
p y&.area

# define_singleton_method under a condition, read through a subclass
class Base
  def self.maybe(f) = (define_singleton_method(:tag) { name } if f; :maybe)
end
class Sub < Base; end
klass = [Base, Sub][1]
p Base.maybe(false), (Base.tag rescue $!.class), (Sub.tag rescue $!.class)
p Base.maybe(true), Base.tag, Sub.tag, klass.tag, Base.respond_to?(:tag)

# the definer is its method's value, but every caller drops it
class Kit
  def self.install(n) = define_method(:seven) { 7 }
  def self.reader = attr_reader(:r)
end
kit = Kit.new
p (kit.seven rescue $!.class), kit.respond_to?(:r)
Kit.install(1)
Kit.reader if true
Kit.install(2) rescue nil
2.times { |i| Kit.install(i) }
p kit.seven, kit.respond_to?(:r), kit.r
