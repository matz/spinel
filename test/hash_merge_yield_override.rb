# spinel: gc-stress

$merge_calls = 0
class Hash
  def merge(other)
    $merge_calls += 1
    :custom
  end
end

def merge_answer
  { "findings" => [] }.merge(yield)
end

p merge_answer { { "entry" => 1 } }
p merge_answer { { marker: 2 } }
puts $merge_calls
