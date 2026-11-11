# A builtin reopening repeated under a condition gets entry functions that
# pick the version that ran; another class's class method of the same name
# keeps its own arm in the entry's dispatch (it called the Numeric instance
# method with no receiver).
class Dur
  def self.seconds(value) = [:dur, value]
end

$loaded = true
class Numeric
  def seconds = [:s, self.class]
  alias :second :seconds
end
unless $loaded
  class Numeric
    def seconds = [:s, self.class]
    alias :second :seconds
  end
end

[1, 2.5].each do |x|
  p x.second
  p x.seconds
end
p Dur.seconds(4)
k = [Dur, 1][0]
begin
  k.seconds
rescue ArgumentError => e
  p e.message
end
