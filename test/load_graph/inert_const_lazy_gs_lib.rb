puts "loaded gs"
junk = []
200.times { |i| junk << ("z" * 64 + i.to_s) }
LGS = "q#{junk.size}"
