# frozen_string_literal: true

source = { "a" => 1, "bb" => 2 }.transform_values do |value|
  next nil if value == 1
  value
end
p source.transform_keys { |key| key.length }
