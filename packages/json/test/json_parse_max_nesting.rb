require "json"

def max_nesting_result(label)
  puts label
  yield
  puts "accepted"
rescue StandardError => error
  puts error.class
  puts error.message
end

[["[", "]"], ['{"a":', "}"]].each do |opening, closing|
  source = opening * 101 + "0" + closing * 101
  [100, 101, false, nil, 0, true, "5"].each do |limit|
    max_nesting_result("#{opening} max_nesting #{limit.inspect}") do
      value = JSON.parse(source, max_nesting: limit)
      101.times { value = value.is_a?(Array) ? value[0] : value["a"] }
      raise "scalar changed" unless value == 0
    end
  end
end
max_nesting_result("custom boundary") { JSON.parse("[[[0]]]", max_nesting: 2) }
max_nesting_result("scalar with limit one") do
  raise "scalar changed" unless JSON.parse("0", max_nesting: 1) == 0
end
max_nesting_result("container with limit one") { JSON.parse("[[0]]", max_nesting: 1) }
max_nesting_result("positional options") { JSON.parse("[[[0]]]", {max_nesting: 2}) }
options = {max_nesting: 2}
max_nesting_result("options variable") { JSON.parse("[[[0]]]", options) }
max_nesting_result("options splat") { JSON.parse("[[[0]]]", **options) }
max_nesting_result("empty options") { JSON.parse("[" * 101 + "0" + "]" * 101, {}) }
max_nesting_result("nil options") { JSON.parse("[" * 101 + "0" + "]" * 101, nil) }
# Spinel already ignores freeze: true. Its presence must not hide max_nesting.
max_nesting_result("ignored option beside nesting") do
  JSON.parse('{"a":[[0]]}', {freeze: true, max_nesting: 2})
end
max_nesting_result("symbolized keys beside nesting") do
  value = JSON.parse('{"a":[0]}', symbolize_names: true, max_nesting: 2)
  raise "symbol key changed" unless value[:a][0] == 0
end
max_nesting_result("symbolized keys past nesting") do
  JSON.parse('{"a":[[0]]}', symbolize_names: true, max_nesting: 2)
end

class NestingKeyOption
  attr_reader :calls
  def initialize
    @calls = 0
  end
  def enabled
    @calls += 1
    @calls == 1
  end
end
max_nesting_result("option evaluated once") do
  switch = NestingKeyOption.new
  value = JSON.parse('{"a":0}', symbolize_names: switch.enabled, max_nesting: 1)
  raise "option evaluated twice" unless switch.calls == 1
  raise "symbol key changed" unless value[:a] == 0
end
max_nesting_result("symbolized options variable") do
  options = {max_nesting: 2, symbolize_names: true}
  value = JSON.parse('{"a":[0]}', options)
  raise "symbol key changed" unless value[:a][0] == 0
end
