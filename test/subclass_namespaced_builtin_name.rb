# A subclass of a builtin Array, Hash, String, ... is refused (#7075), but a
# class of the program's own that shares a builtin's name under a namespace
# is not that builtin: subclassing it is an ordinary program class. Numeric,
# Struct and the exceptions take a subclass as CRuby does.
module Jobs
  class Queue
    def initialize = @items = []
    def push(x) = @items.push(x)
    def size = @items.size
  end

  class Urgent < Queue
    def push(x) = super(x * 10)
  end

  class Range
    def to_s = "jobs range"
  end
end

class Fair < Jobs::Queue; end

class Late < Jobs::Range; end

class Money < Numeric
  def initialize(c) = @c = c
  def cents = @c
end

Pair = Struct.new(:a, :b)
class Sum < Pair
  def total = a + b
end

class Oops < StandardError; end

q = Jobs::Urgent.new
q.push(1)
q.push(2)
p q.size
f = Fair.new
f.push(3)
p f.size
puts Late.new.to_s
p Money.new(5).cents
p Sum.new(1, 2).total
begin
  raise Oops, "boom"
rescue => e
  p [e.class, e.message]
end
