# spinel: not-cruby -- a captured method's String parameter refuses a boxed
# argument that reaches it through `===` on a boxed receiver, through a
# splat, or out of an Array subclass, where CRuby runs the body.
#
# method_capture_boxed_arg.rb is the rule: a boxed argument of `call` or `[]`
# on a boxed receiver reaches the parameters of the captured methods. These
# forms are not yet such evidence, so the Method's gate still raises
# TypeError: `===`, a splat of an Array, a keyword splat of a Hash or of a
# literal, and a Method
# read out of an Array subclass, whose elements' class facts name no Method.
# None converts the value.
def refused(label)
  p yield
rescue TypeError => e
  puts "#{label}: TypeError: #{e.message}"
end
module Policy
  def self.check(value) = value
  def self.listed(value) = value
  def self.named(value:) = value
  def self.literal(value:) = value
  def self.held(value) = value
end
class Held < Array; end
def spread(callback, options) = callback.call(**options)
def splat(callback, list) = callback.call(*list)
Policy.check("a")
Policy.listed("a")
Policy.named(value: "a")
Policy.literal(value: "a")
Policy.held("a")
values = [1.5, :s, 2]
checker = [Policy.method(:check), nil][0]
refused("case equality") { checker === values[0] }
listed = {method: Policy.method(:listed), lambda: ->(value) { value }}
refused("splat") { splat(listed[:method], [values[0]]) }
p splat(listed[:lambda], [values[1]])
named = {method: Policy.method(:named), lambda: ->(value:) { value }}
refused("keyword splat") { spread(named[:method], {value: values[0]}) }
p spread(named[:lambda], {value: values[1]})
literal = {method: Policy.method(:literal)}[:method]
refused("literal keyword splat") { literal.call(**{value: values[0]}) }
held = Held.new
held << Policy.method(:held)
refused("Array subclass") { held[0].call(values[0]) }
p checker === "b", spread(named[:method], {value: "c"}), literal.call(**{value: "d"}), held[0].call("e")
