# Every way the main thread is made runnable again reaches main's own worker
# rather than every worker: a join (main waits for the death in its pump), a
# ConditionVariable signal, a Queue push, a pipe becoming readable, a
# #wakeup ending its sleep, and a timed wait running out. Each is repeated
# with helpers busy and idle around it; a wake that missed main would hang.
def burn(n)
  s = 0
  i = 0
  while i < n
    s = (s * 31 + i) & 0xffff
    i += 1
  end
  s
end

# join: main waits in its pump for each thread's death
tot = 0
100.times do |r|
  ts = Array.new(3) { |w| Thread.new(r * 3 + w) { |k| burn(200 + k % 7) } }
  tot = (tot + ts.sum(&:value)) & 0xffffff
end
p tot

# ConditionVariable: main waits, a helper signals
m = Mutex.new
cv = ConditionVariable.new
box = []
waiter = Thread.new do
  200.times do |i|
    m.synchronize do
      box << burn(50 + i % 5)
      cv.signal
    end
    Thread.pass
  end
end
got = 0
m.synchronize do
  while got < 200
    cv.wait(m) while box.empty?
    got += box.size
    box.clear
  end
end
waiter.join
p got

# Queue: main pops what helpers push
q = Queue.new
producers = Array.new(4) { |w| Thread.new(w) { |k| 50.times { |i| q << k * 1000 + i } } }
sum = 0
200.times { sum += q.pop }
producers.each(&:join)
p sum

# a pipe: main blocks reading, a helper writes
r, w = IO.pipe
writer = Thread.new do
  20.times { |i| w.write("#{i}\n"); burn(100) }
  w.close
end
lines = 0
lines += 1 while r.gets
writer.join
p lines

# #wakeup: main sleeps until a helper wakes it
main = Thread.current
woke = 0
10.times do
  t = Thread.new do
    Thread.pass until main.status == "sleep"
    main.wakeup
  end
  sleep 30
  woke += 1
  t.join
end
p woke

# a timed wait that runs out: the monitor's timer wakes main
q2 = Queue.new
timeouts = 0
5.times { timeouts += 1 if q2.pop(timeout: 0.01).nil? }
p timeouts
