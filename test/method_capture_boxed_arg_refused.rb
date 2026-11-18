# spinel: not-cruby -- a captured method's String parameter refuses a boxed
# argument that reaches it through `===` on a boxed receiver or through a
# splat, where CRuby runs the body with it.
#
# method_capture_boxed_arg.rb is the rule: a boxed argument of `call` or `[]`
# on a boxed receiver reaches the parameters of the captured methods. These
# forms are not yet such evidence, so the Method's gate still raises
# TypeError: `===`, a splat of an Array and a keyword splat of a Hash.
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
end
def spread(callback, options) = callback.call(**options)
def splat(callback, list) = callback.call(*list)
Policy.check("a")
Policy.listed("a")
Policy.named(value: "a")
values = [1.5, :s, 2]
checker = [Policy.method(:check), nil][0]
refused("case equality") { checker === values[0] }
listed = {method: Policy.method(:listed), lambda: ->(value) { value }}
refused("splat") { splat(listed[:method], [values[0]]) }
p splat(listed[:lambda], [values[1]])
named = {method: Policy.method(:named), lambda: ->(value:) { value }}
refused("keyword splat") { spread(named[:method], {value: values[0]}) }
p spread(named[:lambda], {value: values[1]})
p checker === "b", spread(named[:method], {value: "c"})
