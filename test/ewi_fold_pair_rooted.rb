# spinel: gc-stress
# An each.with_index fold whose block takes |acc, pair| builds each step's
# [element, index] pair for the step alone; it is rooted, as a trap the
# block sends itself runs (and allocates) while the pair is in use.
$n = 0
Signal.trap("USR1") { $n += 1; $junk = Array.new(30) { |k| "j" * (k + 40) } }
r = %w[éé ééé éééé ééééé éééééé].each.with_index.inject(0) do |s, pr|
  Process.kill("USR1", Process.pid)
  t = pr[0].length
  s + t * pr[1]
end
p r, $n > 0
