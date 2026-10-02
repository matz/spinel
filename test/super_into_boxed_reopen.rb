# A program class deriving from a builtin reaches the reopening's method of
# the same name by super. The reopening's methods take self boxed; super
# passed it cast to a struct the builtin has none of, and the C did not
# compile. A subclass of Hash or Array (activesupport's
# HashWithIndifferentAccess) is refused now (#7075), so Numeric carries the
# super; the Hash and Array reopenings are still reached on their own values.
class Hash
  def describe(tag) = "#{tag}:#{self.class.name}"
end

class Array
  def describe(tag) = "#{tag}/#{self.class.name}"
end

class Numeric
  def describe(tag) = "#{tag}~#{self.class.name}"
end

class Money < Numeric
  def describe(tag) = "money " + super(tag.upcase)
end

class Cents < Numeric
  def describe(tag) = "cents " + super
end

p Money.new.describe("x")
p Cents.new.describe("y")
p({ a: 1 }.describe("h"))
p [1].describe("a")
p 3.describe("n")
