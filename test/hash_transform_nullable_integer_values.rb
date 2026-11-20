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

# The Integer-valued layout carries nil even for this Hash. A next arm
# still supplies the optional value the collecting block expects.
a = {1 => 1, 2 => 2}
p a.transform_values { |v| v == 1 ? (next v) : v + 2 }
p a.transform_values { |v| next (next nil if v == 1; v + 1) }
