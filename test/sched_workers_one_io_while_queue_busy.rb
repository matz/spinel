# SPINEL_WORKERS=1: main, the one worker, looks at its readiness set while
# threads keep its queue busy, not only once the queue is empty. Pairs of
# threads trade messages over socket pairs, each waiting on a read, while
# other threads stay runnable with Thread.pass; every message arrives, in
# order. Then main, joining a thread that waits on a read, sees it end once
# another thread writes. (Read when the first thread starts; CRuby ignores
# it.)
require "socket"
ENV["SPINEL_WORKERS"] = "1"

$stop = false
spinners = 3.times.map { Thread.new { Thread.pass until $stop } }

pairs = 8.times.map { UNIXSocket.pair }
echoes = pairs.map { |_, b| Thread.new { 200.times { b.write(b.read(4)) } } }
callers = pairs.map.with_index do |(a, _), k|
  Thread.new do
    ok = 0
    200.times do |i|
      msg = format("%04d", (k * 200 + i) % 10000)
      a.write(msg)
      ok += 1 if a.read(4) == msg
    end
    ok
  end
end
p callers.map(&:value)
echoes.each(&:join)
$stop = true
spinners.each(&:join)

r, w = IO.pipe
reader = Thread.new { r.read(5) }
Thread.new { sleep 0.05; w.write("hello") }
p reader.join(5)&.value
