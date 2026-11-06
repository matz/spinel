# `exit` in a thread other than main ended that thread and nothing else: a main
# parked in a wait never heard of it, so the program hung, or deadlocked once
# the last other thread had gone. CRuby raises the SystemExit in the main
# thread, from whatever wait it is in, so its ensures and at_exit hooks run.
require "socket"

def ends(label)
  yield
  puts "#{label}: not raised"
rescue SystemExit => e
  puts "#{label}: exit #{e.status}"
end

def exit_soon(status)
  Thread.new do
    sleep 0.01
    exit(status)
  end
end

ends("sleep") { exit_soon(1); sleep 30 }
ends("queue") { q = Queue.new; exit_soon(2); q.pop }
ends("sized queue") { q = SizedQueue.new(1); q.push(1); exit_soon(3); q.push(2) }
ends("pipe") { r, _w = IO.pipe; exit_soon(4); r.read }
ends("accept") { server = TCPServer.new("127.0.0.1", 0); exit_soon(5); server.accept }
ends("condition variable") do
  lock = Mutex.new
  cond = ConditionVariable.new
  exit_soon(6)
  lock.synchronize { cond.wait(lock) }
end
ends("join of another thread") { other = Thread.new { sleep 30 }; exit_soon(7); other.join }

# A main joining the thread that exits raises it once, not again at its next wait.
ends("join of that thread") { exit_soon(8).join }
ends("the wait after") { sleep 0.05 }

# A rescue inside the thread keeps it to that thread.
kept = Thread.new do
  begin
    exit(9)
  rescue SystemExit => e
    e.status
  end
end
puts "rescued in the thread: #{kept.value}"
ends("nothing to raise") { sleep 0.05 }

# A thread already dead when main joins it: raised once as well, however main
# joins it, and the wait after it has nothing to raise. A later join of that
# thread raises it again, as any thread's exception.
def died_of_exit(status)
  t = Thread.new { exit(status) }
  while t.alive?
  end
  t
end

ends("join of a dead thread") { died_of_exit(10).join }
ends("the wait after") { sleep 0.01 }
ends("value of a dead thread") { died_of_exit(11).value }
ends("the wait after") { sleep 0.01 }
ends("join with a limit of a dead thread") { died_of_exit(12).join(30) }
ends("the wait after") { sleep 0.01 }
ends("join with a limit of that thread") { exit_soon(13).join(30) }
ends("the wait after") { sleep 0.05 }

dead = nil
ends("first join") do
  dead = Thread.new { exit(14) }
  while dead.alive?
  end
  dead.join
end
ends("second join") { dead.join }

# Uncaught, it ends the program as an exit in main does.
at_exit { puts "at_exit ran" }
exit_soon(0)
begin
  sleep 30
ensure
  puts "ensure ran"
end
puts "must not print"
