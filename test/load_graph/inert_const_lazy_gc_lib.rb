puts "loaded gc"
junk = []
200.times { |i| junk << ("z" * 64 + i.to_s) }
LGC = junk.size
