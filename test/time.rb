t = Time.now
puts t.to_f - t.to_i > 0.0
puts t.to_i > 1_000_000_000
puts t.to_i < 2_000_000_000
puts t.to_f > 1_000_000_000.0
puts t.to_i == t.to_f.to_i || t.to_i + 1 == t.to_f.to_i
puts "done"
