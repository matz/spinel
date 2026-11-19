# Keep a mix of Strings and Arrays across collections in a worker pool.
Thread.new { 1 }.join

ts = []
4.times do |t|
  ts << Thread.new do
    strs = []
    objs = []
    i = 0
    while i < 60_000
      strs << "s#{i}-#{t}" * 4
      objs << [i, i + 1]
      strs.shift if strs.length > 2000
      objs.shift if objs.length > 2000
      i += 1
    end
    strs.length + objs.length
  end
end
total = 0
ts.each { |x| total += x.value }
puts total
