# A pool of threads takes jobs in rounds under one Mutex and two
# ConditionVariables, so the mutex is contended on every round: waiters
# count themselves in sp_Mutex_lock under the scheduler lock while the
# unlock fast path reads that count without it. Under scripts/tsan-run.sh
# with SPINEL_WORKERS=2..8 the count's plain increment and decrement were
# reported as data races against that read; they are atomic now. Plain
# `make test` keeps the program honest (the total is deterministic); the
# TSan gate is where the race shows.
m = Mutex.new
work = ConditionVariable.new
done = ConditionVariable.new
pending = 0
finished = 0
total = 0
stop = false
threads = (1..4).map do
  Thread.new do
    running = true
    while running
      got = false
      m.synchronize do
        work.wait(m) while pending == 0 && !stop
        if pending > 0
          pending -= 1
          got = true
        else
          running = false
        end
      end
      if got
        x = 0
        200.times { |k| x += k }
        m.synchronize do
          total += x
          finished += 1
          done.signal
        end
      end
    end
  end
end
50.times do
  m.synchronize do
    finished = 0
    pending = 8
    work.broadcast
    done.wait(m) while finished < 8
  end
end
m.synchronize { stop = true; work.broadcast }
threads.each(&:join)
puts total
