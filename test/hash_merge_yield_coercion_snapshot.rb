# spinel: gc-stress

class Convertible
  def initialize(seed)
    @seed = seed
  end
  def to_hash
    @seed["late"] = true
    {"value" => 9}
  end
end
def wrap(seed)
  seed.merge(yield)
end
seed = {"findings" => []}
p wrap(seed) { {"value" => 7} }.fetch("value")
result = wrap(seed) { Convertible.new(seed) }
p result.key?("late")
p seed.key?("late")
