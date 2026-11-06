# spinel: not-cruby -- The recursive stack guard matches CRuby 4.0.7/json 2.18.
# Newer iterative json parsers can accept the deeper document instead.
require "json"

# A configured limit above the stack guard doesn't lift the guard.
value = JSON.parse("[" * 10_000 + "0" + "]" * 10_000, max_nesting: 100_000)
puts value.is_a?(Array)

begin
  JSON.parse("[" * 100_000 + "0" + "]" * 100_000, max_nesting: 100_000)
  puts false
rescue SystemStackError => error
  puts error.class
end
