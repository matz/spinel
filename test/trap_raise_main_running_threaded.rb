# A trap proc that raises, with a thread alive: the threaded runtime runs the
# proc on its trap thread and hands the raise to the main thread, which must
# take it while running a loop, as CRuby does, not only at its next wait.
trap('INT') { raise Interrupt }
Thread.new { sleep 5 }
Process.spawn('sh', '-c', "sleep 0.3; kill -INT #{Process.pid}")
t0 = Time.now
begin
  x = 0
  x += 1 while Time.now - t0 < 3
  puts "loop ran to the end"
rescue Interrupt
  puts "Interrupt before the end: #{Time.now - t0 < 2}"
end
Process.wait rescue nil
