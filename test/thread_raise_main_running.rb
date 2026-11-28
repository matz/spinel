# Thread#raise aimed at the main thread while it runs (no wait, no
# Thread.pass): the raise lands at main's next safe point, as CRuby raises it
# at the next check, instead of waiting for a wait that may never come.
t0 = Time.now
Thread.new { sleep 0.2; Thread.main.raise(Interrupt) }
begin
  x = 0
  x += 1 while Time.now - t0 < 3
  puts "ran to the end"
rescue Interrupt
  puts "Interrupt before the end: #{Time.now - t0 < 2}"
end

# the same for an exception class with a message
Thread.new { sleep 0.2; Thread.main.raise(RuntimeError, "from a thread") }
begin
  x = 0
  x += 1 while x < 1 << 62
  puts "ran to the end"
rescue RuntimeError => e
  puts e.message
end
