# Threads that never block, more of them than helper workers, all start
# within a few quanta.
#
# With one helper worker (SPINEL_WORKERS=2: main and one helper; CRuby
# ignores it) the helper runs the first spinning thread, and the monitor
# preempts it at the end of each quantum. The other two have not started
# yet, so they wait on the global queue, which the helper must look at
# after such a run instead of picking the spinner back off its own queue:
# they used to wait 61 quanta each, about 0.8 s and 1.5 s.
ENV["SPINEL_WORKERS"] = "2"

t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
stop = false
started = [nil, nil, nil]
spinners = (0...3).map do |i|
  Thread.new(i) do |k|
    started[k] = Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0
    n = 0
    n += 1 until stop
  end
end

deadline = t0 + 0.5
sleep 0.001 until started.all? || Process.clock_gettime(Process::CLOCK_MONOTONIC) > deadline
ok = started.all? { |s| s && s < 0.5 }
stop = true
spinners.each(&:join)

puts(ok ? "all three spinning threads started within 0.5 s" : "late start: #{started.inspect}")
