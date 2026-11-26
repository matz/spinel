# spinel: not-cruby -- CRuby's GVL runs one thread at a time; Spinel places a thread another thread starts on another worker, where it runs beside its sibling
#
# A thread started by another thread is placed as main's threads are
# (#12560): `counter` and `sampler`, both started by `spawner`, first run on
# different workers and stay there. Each waits on `go` as soon as it starts,
# so each is pinned before it does any work. `counter` pushes a tick every
# 1000 turns of its loop; `sampler` reads the number of ticks around a 2 ms
# busy interval of its own, which is shorter than the 100 ms timeslice. The
# two share only Queues, so the ticks grow during the interval only when the
# two run at the same time: on one worker in 0 to 3 of the 20 intervals,
# placed apart in 11 or more under a load of one test per core. The check
# takes 4. Before the change a thread another thread started ran elsewhere
# only when an idle helper stole it before its first run, which happens on a
# lightly loaded machine, so the test pins the placement under load.
ENV["SPINEL_WORKERS"] = "4"
ENV["RUBY_THREAD_TIMESLICE"] = "100"

def now = Process.clock_gettime(Process::CLOCK_MONOTONIC)

spawner = Thread.new do
  ticks = Queue.new
  stop = Queue.new
  ready = Queue.new
  go = Queue.new
  counter = Thread.new do
    ready << :counter
    go.pop
    i = 0
    while stop.empty?
      i += 1
      ticks << i if i % 1000 == 0
    end
  end
  sampler = Thread.new do
    ready << :sampler
    go.pop
    moved = 0
    20.times do
      before = ticks.size
      t = now
      nil until now - t > 0.002
      moved += 1 if ticks.size != before
    end
    moved
  end
  2.times { ready.pop }
  2.times { go << true }
  moved = sampler.join(10)&.value
  stop << true
  counter.join(10)
  moved
end

moved = spawner.join(30)&.value
puts(moved && moved >= 4 ? "the two threads ran at the same time" : "moved in #{moved.inspect} of 20 intervals")
