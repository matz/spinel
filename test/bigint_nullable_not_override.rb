# spinel: int64
# spinel: gc-stress
# Unrelated class and singleton overrides must not change nil or Integer negation.
$receiver_calls = 0
def self.!
  false
end

class Answer
  def !
    false
  end
end

def choose(n)
  $receiver_calls += 1
  if n == 0
    value = nil
  elsif n == 1
    value = 0
  else
    value = 18446744073709551616
  end
  value
end

p !Answer.new
p !choose(0), !choose(1), !choose(2)
p $receiver_calls
