# A signal sent by another process during IO.select runs its trap proc,
# and the select keeps its total timeout across the proc.
class Woken < StandardError; end

def now
  Process.clock_gettime(Process::CLOCK_MONOTONIC)
end

def term_after(delay)
  Process.spawn("sleep #{delay}; kill -TERM #{Process.pid}")
end

r, w = IO.pipe
r2, w2 = IO.pipe

Signal.trap("TERM") { raise Woken }
[
  ["one IO", -> { IO.select([r], nil, nil, 3) }],
  ["two IOs", -> { IO.select([r, r2], nil, nil, 3) }],
  ["error set", -> { IO.select([r], nil, [r], 3) }],
  ["wait_readable", -> { r.wait_readable(3) }],
].each do |label, wait|
  pid = term_after(0.2)
  t0 = now
  begin
    wait.call
    puts "#{label}: returned"
  rescue Woken
    puts "#{label}: trapped"
  end
  puts "#{label}: within 2s #{now - t0 < 2}"
  Process.wait(pid)
end

hits = 0
Signal.trap("TERM") { hits += 1 }
pid = term_after(0.5)
t0 = now
res = IO.select([r], nil, nil, 1.0)
took = now - t0
Process.wait(pid)
puts "counted: #{res.inspect} #{hits}"
puts "total timeout kept: #{took >= 0.9 && took < 1.4}"

# A trap proc that runs for 1 s counts toward the 2 s timeout.
Signal.trap("TERM") { t = now; t = t while now - t < 1.0 }
[
  ["slow trap, one IO", -> { IO.select([r], nil, nil, 2) }],
  ["slow trap, error set", -> { IO.select([r], nil, [r], 2) }],
  ["slow trap, wait_readable", -> { r.wait_readable(2) }],
].each do |label, wait|
  pid = term_after(0.5)
  t0 = now
  res = wait.call
  took = now - t0
  Process.wait(pid)
  puts "#{label}: #{res.inspect} #{took >= 1.9 && took < 2.5}"
end
