# A use that raised on a nil proves the slot non-nil afterwards; these uses
# raise nothing for a nil, so a later read must still test it: `&.` answers
# nil, `==` answers false, a rescued raise goes on, and an `&&` arm that did
# not run tested nothing. Each case runs once with a number and once with
# nil, for a local and for an instance variable.
def try
  yield
rescue NoMethodError, ArgumentError, TypeError => e
  puts "#{e.class}: #{e.message}"
end

def safe(a, i)
  x = a[i]
  r = x&.<(5)
  s = x < 10
  [r, s]
end

def safe_plus(a, i)
  x = a[i]
  y = x&.+(1)
  [y, x.nil?]
end

def equal(a, i)
  x = a[i]
  r = x == 5
  s = x < 10
  [r, s]
end

def rescued(a, i)
  x = a[i]
  begin
    x + 1
  rescue NoMethodError
  end
  x < 10
end

def cond(a, i, b)
  x = a[i]
  t = b && x < 5
  s = x < 10
  [t, s]
end

class Box
  def initialize(v)
    @v = v
  end

  def safe
    r = @v&.<(5)
    s = @v < 10
    [r, s]
  end

  def equal
    r = @v == 5
    s = @v < 10
    [r, s]
  end

  def rescued
    begin
      @v + 1
    rescue NoMethodError
    end
    @v < 10
  end

  def cond(b)
    t = b && @v < 5
    s = @v < 10
    [t, s]
  end
end

ints = [3, 7]
p safe(ints, 0)
try { p safe(ints, 9) }
p safe_plus(ints, 0), safe_plus(ints, 9)
p equal(ints, 0)
try { p equal(ints, 9) }
p rescued(ints, 0)
try { p rescued(ints, 9) }
p cond(ints, 0, true), cond(ints, 1, false)
try { p cond(ints, 9, false) }
try { p cond(ints, 9, true) }

p Box.new(3).safe
try { p Box.new(nil).safe }
p Box.new(3).equal
try { p Box.new(nil).equal }
p Box.new(3).rescued
try { p Box.new(nil).rescued }
p Box.new(3).cond(true), Box.new(7).cond(false)
try { p Box.new(nil).cond(false) }
try { p Box.new(nil).cond(true) }
