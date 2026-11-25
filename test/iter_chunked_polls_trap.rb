# A block loop a region holds polls once per run of up to 1024 passes,
# ahead of its test. A trap's proc runs at that poll, so a proc that empties
# or shifts the array an `each` walks is followed by a test of the new
# length: the walk stops there, with no pass past the end (an in-place read
# past it). Each case runs again until the trap has fired inside a walk.
$stdout.sync = true

def kick
  Process.spawn("sh", "-c", "sleep 0.05; kill -USR1 #{Process.pid}")
end

class R
  attr_accessor :s, :active
  def initialize; @s = 0; @active = false; end

  # (CRuby may run the trap between the length test and the read, which
  # then answers nil: `|| 1` counts that pass as any other, every element
  # being 1, so a pass past the new length shows in @s)
  def walk(a)
    n = 0
    @active = true
    a.each { |x| @s += x || 1; n += 1 }
    @active = false
    n
  end

  def walk_index(a)
    n = 0
    @active = true
    a.each_with_index { |x, i| @s += (x || 1) + (i & 0); n += 1 }
    @active = false
    n
  end
end

$o = R.new

# Runs walk m over a fresh array until the trap, which changes the array
# with `change`, has fired inside a walk (one between two walks proves
# nothing), and answers the passes after it and the walk's own count.
def trapped(m, change)
  loop do
    $a = Array.new(4_000_000, 1)
    $snap = nil
    $inside = false
    Signal.trap(:USR1) { change.call($a); $snap = $o.s; $inside = $o.active }
    k = kick
    last = 0
    last = $o.send(m, $a) while $snap.nil?
    Process.wait(k)
    return [$o.s - $snap, last] if $inside
  end
end

[:walk, :walk_index].each do |m|
  # the trap empties the array the walk reads
  after, last = trapped(m, ->(a) { a.clear })
  p [m, after <= 1, $a.length, last <= 4_000_000]

  # ...or drops all but its last 10 elements
  after, = trapped(m, ->(a) { a.shift(a.length - 10) })
  p [m, after <= 10, $a.length]
end
