# Keep a large live graph of Arrays and Strings while exercising the object
# collector. Each collection visits the graph and frees little.

live = []
120000.times { |i| live << [i, i + 1, i + 2] }
strs = []
20000.times { |i| strs << ("s" * 200 + i.to_s) }
total = 0
3000.times do |k|
  a = [k, k + 1]
  total += a.length
end
puts "#{live.length} #{strs.length} #{total}"
