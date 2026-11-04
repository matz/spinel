# spinel: int64
# spinel: gc-stress
# NilClass owns negation even when the receiver is a nullable Bigint.
class NilClass
  def !; false; end
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
