# spinel: gc-stress
# Literal-name reflection on boxed values keeps its answer per value. Every
# fixed-core tag has absent and present names; shared Boolean tags must not
# mistake either true or false for a class that lacks their public surface.
def probe(value)
  p value.respond_to?(:to_ary)
  p value.respond_to?("to_ary")
  p value.respond_to?(:to_str)
  p value.respond_to?(:zero?)
  p value.respond_to?(:inspect)
  p value.respond_to?(:absent_protocol)
  p value.respond_to?(:upcase)
  p value.respond_to?(:next_float)
  p value.respond_to?(:to_sym)
  p value.respond_to?(:to_a)
  p value.respond_to?(:&)
end

[7, [], "s", {}, nil, true, false, 1.5, :sym].each { |value| probe(value) }

# The receiver is evaluated once even when a tag guard needs its value twice.
$reflection_visits = 0
def reflection_value
  $reflection_visits += 1
  [7, []][$reflection_visits % 2]
end
p reflection_value.respond_to?(:to_ary)
p $reflection_visits

# Dynamic names and include_all retain their existing paths.
def dynamic_probe(value, name, include_all)
  p value.respond_to?(name)
  p value.respond_to?(:to_ary, include_all)
end
[7, []].each { |value| dynamic_probe(value, :to_ary, true) }
