# spinel: gc-stress
$made = 0
class OptionalHash
  def initialize(seed)
    $made += 1
    @seed = seed
  end
  def to_hash(unused = nil)
    @seed["late"] = true
    {"value" => 9, marker: unused}
  end
end
seed = {"seed" => [], marker: 1}
answer = seed.merge(OptionalHash.new(seed))
p answer.fetch("value")
p answer.fetch(:marker)
p answer.key?("late")
p seed.key?("late")
p $made
class EmptyOptionalHash
  def to_hash(unused = nil)
    {"value" => unused.nil? ? 9 : 0, marker: 2}
  end
end
p({"seed" => [], marker: 1}.merge(EmptyOptionalHash.new).fetch("value"))
class KeywordInvalidHash
  def to_hash(unused: nil)
    "invalid"
  end
end
begin
  {"seed" => [], marker: 1}.merge(KeywordInvalidHash.new)
rescue TypeError => error
  puts error.class
end
