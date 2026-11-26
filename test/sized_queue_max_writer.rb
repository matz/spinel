# SizedQueue#max= sets the size and returns it; a larger size lets a pusher
# parked on a full queue go on; a plain Queue has no max=, and a size that
# is not positive raises ArgumentError.
def wait_until
  500.times { break if yield; sleep 0.01 }
end

sq = SizedQueue.new(1)
p(sq.max = 3)
p sq.max

full = SizedQueue.new(1)
full.push(:a)
pusher = Thread.new { full.push(:b); :pushed }
wait_until { full.num_waiting == 1 }
p full.size
full.max = 2
p pusher.join(5)&.value
p full.size, full.max

[0, -1].each do |v|
  begin
    sq.max = v
  rescue ArgumentError => e
    puts "ArgumentError: #{e.message}"
  end
end
p sq.max

[2, "x"].each do |v|
  Queue.new.max = v
rescue NoMethodError => e
  puts "NoMethodError: #{e.message}"
end

["x", nil, :a].each do |v|
  sq.max = v
rescue TypeError => e
  puts "TypeError: #{e.message}"
end
sq.max = 2.7
p sq.max

# a size one larger lets one parked pusher on; the others wait for pops
narrow = SizedQueue.new(1)
narrow.push(0)
pushers = (1..3).map { |i| Thread.new { narrow.push(i) } }
wait_until { narrow.num_waiting == 3 }
narrow.max = 2
wait_until { narrow.size == 2 }
p narrow.size, narrow.num_waiting
got = []
4.times { got << narrow.pop }
pushers.each { |t| t.join(5) }
p got.sort, narrow.size
