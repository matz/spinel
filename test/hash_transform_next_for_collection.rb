# frozen_string_literal: true

numbers = {1 => 1, 2 => 2}
p numbers.transform_values { |value|
  for item in (next nil if value == 1; [value])
  end
  value + 1
}
