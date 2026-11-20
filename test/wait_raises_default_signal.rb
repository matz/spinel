# The default SIGINT and SIGTERM raise in a child wait of a program with one
# thread and no trap, as in CRuby: Interrupt, and the SignalException
# "SIGTERM", where the signal arrived, so the rescue runs then. The wait
# retried waitpid(2) on the EINTR the signal made without looking at the
# recorded signal, and raised it at the next safe point, which came after the
# child had ended, or never: a program whose end follows the wait has none.
def elapsed_since(t0) = Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0

t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
pid = spawn("sh", "-c", "sleep 0.3; kill -INT $PPID; sleep 1.5")
begin
  Process.wait(pid)
  puts "wait returned"
rescue Interrupt => e
  puts "Interrupt in the wait: #{e.message.inspect}, before the child ended: #{elapsed_since(t0) < 1.2}"
end
Process.wait(pid)

t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
begin
  system("sh", "-c", "sleep 0.3; kill -TERM $PPID; sleep 1.5")
  puts "system returned"
rescue SignalException => e
  puts "#{e.class} in system: #{e.message.inspect} #{e.signo}, before the child ended: #{elapsed_since(t0) < 1.2}"
end
Process.wait(-1)
puts "end"
