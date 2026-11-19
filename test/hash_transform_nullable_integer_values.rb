# frozen_string_literal: true

first = +"a"
items = { 1 => first, 2 => +"bb" }
p items.transform_values { |value| value.equal?(first) ? nil : value.length }
p items.transform_values { |value| next if value.equal?(first); value.length }
p items.transform_values { |value| next(value.equal?(first) ? nil : value.length) }
p({ "a" => first, "b" => +"bbb" }.transform_values do |value|
  begin
    next nil if value.equal?(first)
    value.length
  ensure
    GC.start
  end
end)
