# frozen_string_literal: true

def item(value)
  $seen << value
  GC.start
  value
end

$seen = []
result = while true
  begin
    break item(+"word"), key: item(:value)
  ensure
    GC.start
    $seen << :unwind
  end
end
p result, $seen

$seen = []
result = until false
  break item(:first), *[item(:second)], extra: item(:third)
end
p result, $seen
