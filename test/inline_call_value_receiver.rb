# The companion to inline_call_receiver_root.rb, for the receiver that must NOT
# be rooted. A class whose instance variables are all scalars is returned by
# value, so the temporary the inliner hoists the receiver into holds the struct
# itself rather than a pointer to it. Pushing that temporary's address on the
# GC root stack would hand the mark walker the struct's first field, which is
# an integer.
#
# Nothing here can be collected, so this file does not pin a bug the way its
# companion does -- it passes on master too. It is a standing guard on the
# by-value path: these walks must stay correct, and the C this compiles to must
# stay the C master compiles it to, so a change that starts rooting a struct
# receiver shows up as both. It is a file of its own because a class stops
# being by-value as soon as the program pulls in the set package, which is what
# the companion test does.

ENTRIES = 200
CHURN = 120

def churn
  CHURN.times { "q" * 64 }
end

class Span
  def initialize(a, b)
    @a = a
    @b = b
  end

  def each
    i = @a
    while i <= @b
      yield i
      i += 1
    end
    self
  end
end

def make_span = Span.new(1, ENTRIES)

n = 0
tot = 0
make_span.each { |x| churn; n += 1; tot += x }
puts "value receiver n=#{n} tot=#{tot}"

def with_return
  make_span.each do |x|
    churn
    return x if x > 50
  end
  0
end
puts "value receiver return x=#{with_return}"
