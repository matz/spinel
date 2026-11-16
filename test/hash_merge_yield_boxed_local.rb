# spinel: gc-stress
def wrap(seed)
  seed.merge(yield)
end
class Convertible
  def initialize(seed); @seed = seed; end
  def to_hash
    @seed["late"] = true
    {"value" => 9}
  end
end
seed = {"findings" => []}
p wrap(seed) { {"value" => 7} }.fetch("value")
result = wrap(seed) { value = Convertible.new(seed); value = {"value" => 2} unless ARGV.empty?; value }
p result.fetch("value")
p result.key?("late")
p seed.key?("late")
