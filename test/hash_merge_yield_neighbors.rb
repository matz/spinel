# spinel: gc-stress

class StringHashLike
  def to_hash
    { "converted" => 7 }
  end
end

class SymbolHashLike
  def to_hash
    { converted: 9 }
  end
end

def merge_answer
  source = { "findings" => [] }.freeze
  result = source.merge(yield)
  copy = result
  copy
end

puts merge_answer { { "entry" => 1 } }.fetch("entry")
p merge_answer { { marker: nil } }.fetch(:marker)
puts merge_answer { { 3 => "integer key" } }.fetch(3)
puts merge_answer { StringHashLike.new }.fetch("converted")
puts merge_answer { SymbolHashLike.new }.fetch(:converted)
begin
  merge_answer { nil }
rescue TypeError => error
  puts error.class
end
puts merge_answer { { "outcome" => "frozen" }.freeze }.fetch("outcome")
answer = merge_answer { { "value" => [1] } }
copy = answer
copy.fetch("value") << 2
p answer.fetch("value")
