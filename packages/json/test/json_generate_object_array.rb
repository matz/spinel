require "json"

$calls = 0
class Label
  def initialize(named) = @named = named
  def to_s
    $calls += 1
    @named ? "yes" : nil
  end
end

# Both calls receive a typed Array of program objects, and each element's
# own to_s is called once. A to_s that answers no String is CRuby's
# TypeError, as Object#to_json takes it (an interpolation would fall back
# to the default text instead).
puts JSON.generate([Label.new(true)])
begin
  puts JSON.generate([Label.new(false)])
rescue TypeError => e
  p [e.class, e.message]
end
p $calls
