# The boxed argument of method_capture_boxed_arg.rb, into an instance method
# and into one a subclass inherits, each typed by a direct call first.
class Base
  def initialize(tag) = @tag = tag
  def label(value) = "#{@tag}:#{value.inspect}"
  def twice(value) = value * 2
end
class Child < Base
end
def run(options, value)
  options[:handler].call(value)
end
b = Base.new("b")
c = Child.new("c")
p b.label(1)
p c.twice(2)
p run({handler: b.method(:label)}, 1.5)
p run({handler: c.method(:label)}, "s")
p run({handler: c.method(:twice)}, 2.5)
p run({handler: c.method(:twice)}, "ab")
p run({handler: ->(v) { v }}, :x)
p b.label(4)
p c.twice(5) + 1
