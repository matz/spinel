# respond_to? and method_defined? on a program class ask the call plan for the
# class's entry, its visibility and its undef; the names a mixin or a Struct
# brings have no entry. A typed receiver, a boxed one over several classes and
# a subclass slot all answer from the same facts.
module Tagged
  def tag; end
end

class Base
  include Enumerable
  include Tagged
  def each; yield 1; end
  def shown; end
  def secret; end
  private :secret
  protected def guarded; end
  attr_reader :rd
  attr_writer :wr
  alias_method :shown2, :shown
  alias_method :old_to_s, :to_s
end

class Sub < Base
  undef_method :shown
  def secret; end
  def extra; end
end

Pair = Struct.new(:a, :b) do
  def total = a + b
end
Dot = Data.define(:x, :y)

class Order
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
end

def row(o, names)
  names.map { |n| [o.respond_to?(n), o.respond_to?(n, true)].map { |v| v ? 1 : 0 }.join }.join(" ")
end

names = %i[each map shown shown2 old_to_s secret guarded rd wr wr= tag extra to_a clamp members with total a x]
[Base.new, Sub.new, Pair.new(1, 2), Dot.new(1, 2), Order.new(1)].each do |o|
  puts "#{o.class}: #{row(o, names)}"
end

pick = [Base.new, Sub.new, Pair.new(1, 2), Order.new(2), Dot.new(1, 2)]
names.each do |n|
  puts "#{n}: #{pick.map { |o| o.respond_to?(n) ? 1 : 0 }.join} #{pick.map { |o| o.respond_to?(n, true) ? 1 : 0 }.join}"
end

puts "Base: #{[Base.method_defined?(:each), Base.method_defined?(:shown), Base.public_method_defined?(:secret),
                    Base.private_method_defined?(:secret), Base.protected_method_defined?(:guarded), Base.method_defined?(:rd),
                    Base.method_defined?(:wr=), Base.method_defined?(:tag), Base.method_defined?(:shown2, false),
                    Base.method_defined?(:tag, false)].map { |v| v ? 1 : 0 }.join}"
puts "Sub: #{[Sub.method_defined?(:shown), Sub.method_defined?(:shown2), Sub.method_defined?(:extra, false),
                   Sub.method_defined?(:each, false), Sub.public_method_defined?(:secret), Sub.private_method_defined?(:secret),
                   Sub.method_defined?(:rd), Sub.method_defined?(:old_to_s)].map { |v| v ? 1 : 0 }.join}"
puts "Pair: #{[Pair.method_defined?(:a), Pair.method_defined?(:a=), Pair.method_defined?(:total), Pair.method_defined?(:total, false),
                    Pair.method_defined?(:each), Pair.method_defined?(:members), Pair.method_defined?(:a, false)].map { |v| v ? 1 : 0 }.join}"
puts "Dot: #{[Dot.method_defined?(:x), Dot.method_defined?(:x=), Dot.method_defined?(:with), Dot.method_defined?(:each),
                   Dot.method_defined?(:x, false)].map { |v| v ? 1 : 0 }.join}"
puts "Order: #{[Order.method_defined?(:<=>), Order.method_defined?(:n), Order.method_defined?(:n, false),
                     Order.method_defined?(:between?, false)].map { |v| v ? 1 : 0 }.join}"
