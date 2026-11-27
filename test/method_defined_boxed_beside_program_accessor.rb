# A `public_method_defined?` on a class only the run time knows (a class held
# in a variable) is answered from the reflection table. When the program also
# defines a method of that name (an options object's accessor) and the
# receiver's slot says nothing of what it holds, the dispatch may land on
# either, so the call's value is boxed: the reflection's answer has to be
# boxed to match, wherever the value goes (a block's truth test, a proc's
# return).
class Req
  def bar; end
end
class Other
  def baz; end
end
class Options
  def initialize(h) = @h = h
  def [](k) = @h[k]
  def public_method_defined?(name) = self[name]
  def method_defined?(name) = self[name]
end
class Mapping
  def initialize(set, conditions); @set = set; @conditions = conditions; end
  def conditions = build_conditions(@conditions, @set.request_class)
  def build_conditions(current_conditions, request_class)
    conditions = current_conditions.dup
    conditions.keep_if do |k, _|
      request_class.public_method_defined?(k)
    end
  end
  def checker(request_class) = proc { |k| request_class.method_defined?(k) }
  def answers(request_class, names) = names.map { |k| request_class.public_method_defined?(k) }
end
class Set1
  attr_accessor :request_class
  def initialize; @request_class = Req; end
end
s = Set1.new
s.request_class = Other if ARGV[0]
m = Mapping.new(s, {})
args = [{ bar: 1, baz: 2 }, s.request_class]
p m.build_conditions(*args)
chk = m.checker(*[s.request_class])
p chk.call(:bar), chk.call(:baz)
p m.answers(s.request_class, [:bar, :baz, "bar"])
o = Options.new(bar: true, baz: false)
p o[:bar]
