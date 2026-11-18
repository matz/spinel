# spinel: not-cruby -- a captured method's String parameter refuses a boxed
# argument that reaches it through `===` on a boxed receiver, through a
# splat, or out of an Array subclass, where CRuby runs the body.
#
# method_capture_boxed_arg.rb is the rule: a boxed argument of `call` or `[]`
# on a boxed receiver reaches the parameters of the captured methods. These
# forms are not yet such evidence, so the Method's gate still raises
# TypeError: `===`, a splat of an Array, a keyword whose key is computed,
# a keyword splat of a Hash or of a literal, and a Method
# read out of an Array subclass, whose elements' class facts name no Method.
# None converts the value.
#
# `super_method` of a Method local that may hold more than one method has no
# one parent to bind, and raises NotImplementedError where CRuby answers.
def refused(label)
  p yield
rescue TypeError => e
  puts "#{label}: TypeError: #{e.message}"
end
module Policy
  def self.check(value) = value
  def self.listed(value) = value
  def self.named(value:) = value
  def self.computed(value:) = value
  def self.literal(value:) = value
  def self.held(value) = value
end
class Held < Array; end
def spread(callback, options) = callback.call(**options)
def splat(callback, list) = callback.call(*list)
Policy.check("a")
Policy.listed("a")
Policy.named(value: "a")
Policy.computed(value: "a")
Policy.literal(value: "a")
Policy.held("a")
values = [1.5, :s, 2]
checker = [Policy.method(:check), nil][0]
refused("case equality") { checker === values[0] }
listed = {method: Policy.method(:listed), lambda: ->(value) { value }}
refused("splat") { splat(listed[:method], [values[0]]) }
p splat(listed[:lambda], [values[1]])
computed = {method: Policy.method(:computed)}[:method]
keys = [:value]
refused("computed keyword") { computed.call(keys[0] => values[0]) }
named = {method: Policy.method(:named), lambda: ->(value:) { value }}
refused("keyword splat") { spread(named[:method], {value: values[0]}) }
p spread(named[:lambda], {value: values[1]})
literal = {method: Policy.method(:literal)}[:method]
refused("literal keyword splat") { literal.call(**{value: values[0]}) }
held = Held.new
held << Policy.method(:held)
refused("Array subclass") { held[0].call(values[0]) }
p checker === "b", spread(named[:method], {value: "c"}), literal.call(**{value: "d"}), held[0].call("e")
class Parent
  def f(x) = [:parent_f, x]
end
class Child < Parent
  def f(x) = [:child_f, x]
end
def parent_of(m, flag)
  m = Child.new.method(:f) if flag
  m.super_method
end
begin
  p parent_of(Child.new.method(:f), false).call(1)
rescue NotImplementedError
  puts "super_method: NotImplementedError"
end
# The same through a copy of the local: `m.dup` holds what `m` holds.
def parent_of_copy(m, flag)
  m = Child.new.method(:f) if flag
  [m.dup.super_method, m.clone.super_method]
end
begin
  p parent_of_copy(Child.new.method(:f), false).length
rescue NotImplementedError
  puts "dup.super_method: NotImplementedError"
end
