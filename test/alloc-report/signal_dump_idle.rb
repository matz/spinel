# The idle case, which is the shape of every server this report exists for: a
# program that has allocated, is now parked with nothing to do, and is asked
# for its numbers. The dump must arrive when asked rather than waiting for the
# next allocation -- an idle server has none.
worker = Thread.new do
  s = "warm-up"
  i = 0
  while i < 100
    s = "item-#{i}"
    i += 1
  end
  puts s.length
  $stdout.flush   # the Makefile waits for this line before it signals
  sleep 300
end
worker.join
