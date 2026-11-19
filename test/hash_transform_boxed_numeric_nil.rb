# frozen_string_literal: true

numbers = {1 => 1, 2 => 2, 3 => 3}
p numbers.transform_values { |n| n.even? ? n + 0.25 : nil }
p numbers.transform_values { |n|
  begin
    next nil if n == 1
    next n + 0.25 if n == 2
    -0.0
  ensure
    GC.start
  end
}
p({1 => 1, :two => 2}.transform_values { |n| next if n == 1; n })
