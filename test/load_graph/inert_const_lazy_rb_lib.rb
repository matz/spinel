puts "loaded rb"
$s = +"zz"
junk = []
200.times { |i| junk << ("z" * 64 + i.to_s) }
LRB = junk.size
