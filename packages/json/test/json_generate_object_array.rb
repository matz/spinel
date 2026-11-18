require "json"

$calls = 0
class Label
  def initialize(named) = @named = named
  def to_s
    $calls += 1
    @named ? "yes" : nil
  end
end

# Both calls receive a typed Array of program objects. A nil return from
# to_s falls back to the object's default text within the same Array.
puts JSON.generate([Label.new(true)])
puts JSON.generate([Label.new(false)]).gsub(/0x[0-9a-f]+/, "ADDR")
p $calls
