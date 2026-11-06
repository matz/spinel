# spinel: int64
# spinel: gc-stress
# A Kernel prepend also owns negation of a nullable Bigint.
module Negation
  def !; false; end
end
module Kernel
  prepend Negation
end
$calls = 0
def choose(n)
  $calls += 1
  if n == 0
    value = nil
  elsif n == 1
    value = 0
  else
    value = 18446744073709551616
  end
  value
end
p !choose(0), !choose(1), !choose(2)
p $calls
