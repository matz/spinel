begin
  p [[1]].dig(0, :x)
rescue => e
  p [e.class, e.message]
end

def nested(key)
  [[1]].dig(0, key)
end
["x", nil, true, false].each do |key|
  begin
    p nested(key)
  rescue => e
    p [e.class, e.message]
  end
end
p [[1, 2]].dig(0, -1)
p [[1, 2]].dig(0, 1.9)
p [{x: [3]}].dig(0, :x, 0)
p [[{x: 4}]].dig(0, 0, :x)
p [nil].dig(0, :x)
p [[1]].dig(8, :x)
keys = [0, :x]
begin
  p [[1]].dig(*keys)
rescue => e
  p [e.class, e.message]
end
class Index
  def to_int
    print "I"
    0
  end
end
p [[9]].dig(0, Index.new)
