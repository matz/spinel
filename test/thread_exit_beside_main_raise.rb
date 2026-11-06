# A raise aimed at main while an exit in another thread is on its way to main
# took the exit's place in the one slot main has: the rescue below caught it
# and main went on, with nothing left to end it.
at_exit { puts "at_exit ran" }

m = Thread.current
t1 = Thread.new { exit 4 }
t2 = Thread.new do
  while t1.alive?
  end
  m.raise("boom")
end

begin
  while t2.alive?
  end
  sleep 0.05
rescue RuntimeError => e
  puts "rescued #{e.message}"
end
puts "must not print"
