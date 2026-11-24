# A block loop a region holds polls once per run of up to 1024 passes,
# ahead of its test. A trap's proc runs at that poll, so a proc that empties
# or shifts the array an `each` walks is followed by a test of the new
# length: the walk stops there, with no pass past the end (an in-place read
# past it). Each walk runs until the trap has fired inside it.
$stdout.sync = true

def kick
  Process.spawn("sh", "-c", "sleep 0.05; kill -USR1 #{Process.pid}")
end

class R
  attr_accessor :s
  def initialize; @s = 0; end

  # (CRuby may run the trap between the length test and the read, which
  # then answers nil)
  def walk(a)
    n = 0
    a.each { |x| @s += x || 0; n += 1 }
    n
  end

  def walk_index(a)
    n = 0
    a.each_with_index { |x, i| @s += (x || 0) + (i & 0); n += 1 }
    n
  end
end

$o = R.new

[:walk, :walk_index].each do |m|
  # the trap empties the array the walk reads
  $a = Array.new(4_000_000, 1)
  $snap = nil
  Signal.trap(:USR1) { $a.clear; $snap = $o.s }
  k = kick
  last = 0
  last = $o.send(m, $a) while $snap.nil?
  Process.wait(k)
  p [m, $o.s - $snap <= 1, $a.length, last >= 0]

  # ...or drops all but its last 10 elements
  $a = Array.new(4_000_000, 1)
  $snap = nil
  Signal.trap(:USR1) { $a.shift($a.length - 10); $snap = $o.s }
  k = kick
  last = $o.send(m, $a) while $snap.nil?
  Process.wait(k)
  p [m, $o.s - $snap <= 10, $a.length]
end
