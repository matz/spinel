# Time#strftime from threads running at once: each thread gets its own
# result (the formatter's buffer is per thread) (#11352)
f = ->(i) { Time.utc(2026, 1 + (i % 12), 1 + (i % 28), i % 24, i % 60, i % 60).strftime('%Y-%m-%dT%H:%M:%S') }
ref = Array.new(2000) { |i| f.call(i) }
par = Array.new(8) { Thread.new { Array.new(2000) { |i| f.call(i) } } }.map(&:value)
bad = par.sum { |arr| arr.each_with_index.count { |v, i| v != ref[i] } }
puts "mismatches=#{bad}"
