# SPINEL_WORKERS=1, main parked, a sibling thread spinning on Thread.pass: the
# signal's proc is main's (the trap thread hands it a raise), not the
# sibling's, which polls the same flag; before, the sibling could take the
# signal, run the proc and die of its raise, and main never saw it.
ENV['SPINEL_WORKERS'] = '1'
trap('INT') { raise Interrupt }
sibling = Thread.new { loop { Thread.pass } }
Process.spawn('sh', '-c', "sleep 0.3; kill -INT #{Process.pid}")
t0 = Time.now
begin
  sleep 3
  puts "main slept to the end"
rescue Interrupt
  puts "Interrupt in main before the end: #{Time.now - t0 < 2}"
end
puts "sibling alive: #{sibling.alive?}"
sibling.kill
Process.wait
