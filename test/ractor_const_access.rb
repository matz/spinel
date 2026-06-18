# A Ractor block must be self-contained: it may NOT access outer local
# variables (that is a compile-time Ractor::IsolationError, matching CRuby's
# ArgumentError), but it MAY reference shareable constants, which are shared
# read-only across Ractors (#1456 / CRuby-faithful).
BASE = 100
FACTOR = 3
NUMS = [1, 2, 3, 4].freeze

r = Ractor.new do
  BASE * FACTOR
end
puts r.take

s = Ractor.new do
  NUMS.sum + BASE
end
puts s.take

# Constants combined with a spawn argument (the supported way to pass dynamic
# data into a Ractor).
t = Ractor.new(7) do |n|
  n * FACTOR
end
puts t.take
