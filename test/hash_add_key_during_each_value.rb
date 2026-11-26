# A new key stored inside each_value, each_key or each_pair raises the
# RuntimeError each raises; storing over an existing key does not
h = {1 => :a, 2 => :b}
[:each_value, :each_key, :each_pair].each do |m|
  begin
    case m
    when :each_value then h.each_value { |v| h.store(9, v) }
    when :each_key then h.each_key { |k| h[k + 10] = :x }
    else h.each_pair { |k, v| h[k + 20] = v }
    end
  rescue RuntimeError => e
    puts "#{m}: #{e.message}"
  end
end

g = {1 => 10, 2 => 20}
g.each_value { |v| g[1] = v + 1 }
p g
seen = []
g.each_key do |k|
  next if k == 1
  seen << k
end
p seen
d = {1 => :a, 2 => :b, 3 => :c}
d.each_key { |k| d.delete(k) if k == 2 }
p d.keys
