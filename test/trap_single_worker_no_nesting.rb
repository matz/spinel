# SPINEL_WORKERS=1: a second delivery of a signal while its proc runs waits
# for the proc, as CRuby runs the handlers of a signal one after the other,
# instead of starting the proc inside itself at a poll of the proc's own.
ENV['SPINEL_WORKERS'] = '1'
$depth = 0
$deepest = 0
$runs = 0
trap('INT') do
  $depth += 1
  $deepest = $depth if $depth > $deepest
  t = Time.now
  x = 0
  x += 1 while Time.now - t < 0.4
  $runs += 1
  $depth -= 1
end
Thread.new { sleep 5 }
Process.spawn('sh', '-c', "sleep 0.3; kill -INT #{Process.pid}; sleep 0.1; kill -INT #{Process.pid}")
t0 = Time.now
x = 0
x += 1 while $runs < 2 && Time.now - t0 < 3
puts "runs=#{$runs} deepest=#{$deepest}"
Process.wait
