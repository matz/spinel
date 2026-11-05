# spinel: not-cruby -- The recursive stack guard matches CRuby 4.0.7/json 2.18.
# Newer iterative json parsers can accept the deeper document instead.
require "json"

value = JSON.parse("[" * 10_000 + "0" + "]" * 10_000, max_nesting: false)
depth = 0
while value.is_a?(Array) && value.length == 1
  depth += 1
  value = value[0]
end
puts depth == 10_000
puts value == 0

begin
  JSON.parse("[" * 100_000 + "0" + "]" * 100_000, max_nesting: false)
  puts false
rescue SystemStackError => error
  puts error.class
  puts error.message == "stack level too deep"
end
