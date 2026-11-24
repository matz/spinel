# A chunked loop polls once per run of up to 1024 passes, ahead of its
# test. A trap's proc runs at that poll, so a proc that empties the array a
# proved loop counts to, or lowers the bound a counted loop counts to, is
# followed by a test of the new length or bound: the loop stops there, with
# no pass past the end (an in-place read past it) and no run of passes past
# the new bound. Each loop runs until the trap has fired inside it.
$stdout.sync = true

def kick
  Process.spawn("sh", "-c", "sleep 0.05; kill -USR1 #{Process.pid}")
end

class R
  attr_accessor :n, :cur, :s
  def initialize; @n = 0; @cur = 0; @s = 0; end

  # proved: i < a.length, reading a[i] in place (CRuby may run the trap
  # between the test and the read, which then answers nil)
  def proved(a)
    i = 0
    while i < a.length
      @s += a[i] || 0
      i += 1
    end
    i
  end

  # counted to an instance variable's bound, reading an array
  def counted(a)
    j = 0
    while j < @n
      @cur = j
      @s += a[j & 1023]
      j += 1
    end
    j
  end

  # counted to an instance variable's bound, scalars only
  def pure
    j = 0
    while j < @n
      @cur = j
      j += 1
    end
    j
  end
end

$o = R.new
$a = nil
$snap = nil
$at = nil

# the trap empties the array the proved loop counts to
$a = Array.new(4_000_000, 1)
Signal.trap(:USR1) { $a.clear; $snap = $o.s }
k = kick
last = 0
last = $o.proved($a) while $snap.nil?
Process.wait(k)
p [$o.s - $snap <= 1, $a.length, last >= 0]

# ...or drops all but its last 10 elements
$a = Array.new(4_000_000, 1)
$snap = nil
Signal.trap(:USR1) { $a.shift($a.length - 10); $snap = $o.s }
k = kick
last = $o.proved($a) while $snap.nil?
Process.wait(k)
p [$o.s - $snap <= 10, $a.length]

# the trap lowers the bound the counted loops count to
[:counted, :pure].each do |m|
  $at = nil
  Signal.trap(:USR1) { $o.n = 0; $at = $o.cur }
  k = kick
  arr = Array.new(1024, 1)
  while $at.nil?
    $o.n = 400_000_000
    last = m == :counted ? $o.counted(arr) : $o.pure
  end
  Process.wait(k)
  p [m, last - $at <= 2]
end
