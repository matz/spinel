# SPINEL_WORKERS=1 (what Fluxpinel asks for): main is the one worker, and a
# thread of the runtime runs only when main pumps, in a wait. A Signal.trap
# proc then has to run on the main thread itself while main computes, at its
# next poll, as CRuby runs every trap; before, a signal during a loop of
# main's waited for main's next wait.
ENV['SPINEL_WORKERS'] = '1'
got = nil
t0 = Time.now
trap('INT') { got = Time.now - t0 }
Thread.new { sleep 5 }
Process.spawn('sh', '-c', "sleep 0.3; kill -INT #{Process.pid}")
x = 0
x += 1 while got.nil? && Time.now - t0 < 3
puts got ? "the proc ran while main computed: #{got < 2}" : "the proc did not run"
Process.wait

# and a raise out of the proc reaches the loop
trap('INT') { raise Interrupt }
Process.spawn('sh', '-c', "sleep 0.3; kill -INT #{Process.pid}")
t0 = Time.now
begin
  x += 1 while Time.now - t0 < 3
  puts "loop ran to the end"
rescue Interrupt
  puts "Interrupt before the end: #{Time.now - t0 < 2}"
end
Process.wait
