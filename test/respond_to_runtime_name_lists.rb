# respond_to? with a name only the run time knows asks each class's names, as
# the call plan holds them: the class's own methods, its accessors and
# aliases, the names a mixin or a Struct brings, the Object private names
# include_all adds, and what an undef_method takes away.
class Plain
  def pub; end
  def hidden; end
  private :hidden
  attr_accessor :acc
  alias_method :alias_pub, :pub
end

class Child < Plain
  undef_method :pub
  def own; end
end

class Gone
  undef_method :puts
  undef_method :exit
end

class Bag
  include Enumerable
  def each; yield 1; end
end

class Rank
  include Comparable
  def <=>(o) = 0
end

Pair = Struct.new(:a, :b)
Dot = Data.define(:x)

names = %w[pub hidden acc acc= alias_pub own puts exit map between? members to_a with zork initialize]
objs = [Plain.new, Child.new, Gone.new, Bag.new, Rank.new, Pair.new(1, 2), Dot.new(1)]
objs.each do |o|
  row = names.map { |n| [o.respond_to?(n), o.respond_to?(n, true)] }
  puts "#{o.class}: #{row.map { |a, b| (a ? "1" : "0") + (b ? "1" : "0") }.join(" ")}"
end

# a boxed receiver goes through the same lists
mixed = [Plain.new, 3, "s", nil, Gone.new, Bag.new]
names.each do |n|
  puts "#{n}: #{mixed.map { |o| o.respond_to?(n) ? 1 : 0 }.join} #{mixed.map { |o| o.respond_to?(n, true) ? 1 : 0 }.join}"
end

# method_defined? with a name or class only the run time knows
[Plain, Child, Gone, Bag, Pair, Dot].each do |k|
  puts "#{k}: " + %w[pub hidden acc acc= own].map { |n|
    (k.method_defined?(n) ? "1" : "0") + (k.private_method_defined?(n) ? "1" : "0")
  }.join(" ")
end
