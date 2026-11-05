require "json"

# Nest around a scalar: json releases differ on an empty innermost container.
def nesting_result(label)
  puts label
  yield
  puts "accepted"
rescue JSON::ParserError => error
  puts error.class
  puts error.message
end

[["[", "]"], ['{"a":', "}"]].each do |opening, closing|
  [100, 101, 201].each do |depth|
    nesting_result("#{opening} depth #{depth}") do
      value = JSON.parse(opening * depth + "0" + closing * depth)
      depth.times { value = value.is_a?(Array) ? value[0] : value["a"] }
      raise "scalar changed" unless value == 0
    end
  end
end

# NestingError must still be caught through both JSON parent classes.
begin
  JSON.parse("[" * 101 + "0" + "]" * 101)
rescue JSON::JSONError => error
  puts error.class
  puts error.message
end
puts JSON::NestingError.ancestors.first(3)
