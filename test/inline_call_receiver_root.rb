# A call to a user-defined method that yields is inlined at the call site, and
# the call's receiver is copied into a C temporary that the whole inlined body
# then reads its ivars through. For a receiver no Ruby name holds -- a method's
# answer, a constructor, a chain -- that temporary is the only reference to the
# object, while the body allocates and so does the caller's block at every
# yield. Every arm below hands one of those a fresh receiver and allocates
# inside the block before it looks at what it was given, so an arm whose
# receiver went away mid-walk reports a short count instead of passing quietly.
# The plain local-receiver arm is the control, and it was always safe: the
# local is a root of its own. The arm after it is not a control -- a local
# stops holding the object the moment the block rebinds it, and from there the
# hoisted temporary is again the only reference, so that walk truncates on a
# tree without this fix as surely as a fresh receiver does. A receiver
# returned by value has its own test, because a class only stays by-value in a
# program that does not pull in the set package.

require "set"

ENTRIES = 200
CHURN = 120

def churn
  CHURN.times { "q" * 64 }
end

class Ledger
  def initialize(n)
    @rows = []
    (1..n).each { |i| @rows << i }
  end

  def each
    i = 0
    while i < @rows.length
      yield @rows[i]
      i += 1
    end
    self
  end

  def each_scaled(k)
    i = 0
    while i < @rows.length
      yield @rows[i] * k
      i += 1
    end
    self
  end

  def size = @rows.length
end

def make_ledger = Ledger.new(ENTRIES)
def make_set = (1..ENTRIES).map { |i| "s#{i}" }.to_set

# --- a hand-written each, receiver held only by the hoisted temporary ---

n = 0
make_ledger.each { |x| churn; n += 1 }
puts "ledger each n=#{n}"

# --- the same, in expression position: the inline is a statement expression ---

n = 0
r = make_ledger.each { |x| churn; n += 1 }
puts "ledger each value n=#{n} size=#{r.size}"

# --- Set#each, which is a Ruby-level each in the bundled set package ---

n = 0
last = nil
make_set.each { |s| churn; n += 1; last = s }
puts "set each n=#{n} last=#{last}"

# --- a nested inline: the yielded block inlines another call, and both
#     receivers are fresh, so the roots have to nest strictly ---

outer = 0
inner = 0
Ledger.new(20).each do |a|
  Ledger.new(10).each do |b|
    churn
    inner += b
  end
  outer += a
end
puts "nested outer=#{outer} inner=#{inner}"

# --- leaving the inlined body early: break, next, return, raise ---

def with_break
  n = 0
  make_ledger.each do |x|
    churn
    break if x > 100
    n += 1
  end
  n
end
puts "break n=#{with_break}"

def with_next
  n = 0
  make_ledger.each do |x|
    churn
    next if x.even?
    n += 1
  end
  n
end
puts "next n=#{with_next}"

def with_return
  make_ledger.each do |x|
    churn
    return x if x > 50
  end
  0
end
puts "return x=#{with_return}"

def with_raise
  n = 0
  begin
    make_ledger.each do |x|
      churn
      raise "stop" if x > 70
      n += 1
    end
  rescue RuntimeError => e
    return "#{n} #{e.message}"
  end
  "no raise"
end
puts "raise #{with_raise}"

# --- the argument list is call-site code evaluated after the receiver is
#     hoisted, and evaluating it allocates ---

n = 0
tot = 0
make_ledger.each_scaled(("ab" * 3).size / 2) { |v| churn; n += 1; tot += v }
puts "scaled n=#{n} tot=#{tot}"

# --- a block forwarded with & rather than written at the call site ---

def each_fresh(&blk)
  make_ledger.each(&blk)
end

n = 0
each_fresh { |x| churn; n += 1 }
puts "forwarded n=#{n}"

# --- controls ---

led = make_ledger
n = 0
led.each { |x| churn; n += 1 }
puts "local receiver n=#{n}"

# --- ... but only while the local still names it: once the block clears the
#     local, the hoisted temporary is the only reference again ---

led = make_ledger
n = 0
led.each do |x|
  churn
  n += 1
  led = nil if x == 3
end
puts "rebound receiver n=#{n} led=#{led.inspect}"
