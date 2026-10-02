# A program class deriving from Hash or Array reaches the reopening's method
# of the same name by super -- activesupport's HashWithIndifferentAccess
# does for reverse_merge and slice!. The reopening's methods take self
# boxed; super passed it cast to a struct the builtin has none of, and the
# C did not compile.
class Hash
  def describe(tag) = "#{tag}:#{self.class.name}"
end

class Array
  def describe(tag) = "#{tag}/#{self.class.name}"
end

class Opts < Hash
  def describe(tag) = "opts " + super(tag.upcase)
end

class List < Array
  def describe(tag) = "list " + super
end

p Opts.new.describe("x")
p List.new.describe("y")
p({ a: 1 }.describe("h"))
p [1].describe("a")
