# spinel: not-cruby -- ffi_func is Spinel's own declaration
# A thread spawned just before main's Thread.pass stays a helper's to run. The
# pass used to find the new helper not up yet and run the thread on main's
# worker, which pinned it there for life: from then on it ran only while main
# was at a wait of the scheduler, and stood still through a blocking C call.
module C
  ffi_func :usleep, [:uint32], :int, blocking: true
end

$stop = false
counts = [0, 0, 0, 0]
threads = (0...4).map do |i|
  t = Thread.new(i) do |w|
    until $stop
      counts[w] += ("x" * 8).length
    end
  end
  Thread.pass
  t
end

# Main stays in C but for a look every 100 ms. Each thread moves on meanwhile.
before = counts.dup
moved = [false, false, false, false]
300.times do
  C.usleep(100_000)
  4.times { |i| moved[i] = true if counts[i] > before[i] }
  break if moved.all?
end
p moved

$stop = true
threads.each(&:join)
puts "joined"
