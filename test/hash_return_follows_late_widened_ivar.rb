# A method that answers a Hash literal over an ivar follows the ivar when it
# is widened only after the method's return was derived. An attr writer given
# the answer of a block call nothing resolves does that: the answer has no
# type until the late widening boxes it, the ivar is boxed with it, and the
# literal then builds the boxed-value Hash. The return stayed
# Hash[String, Integer], so the C returned an sp_StrPolyHash * through an
# sp_StrIntHash * and did not build.

class Tally
  attr_accessor :count
  attr_writer :label

  def initialize
    @count = 0
    @label = "none"
    @code = 7
  end

  def to_h
    { "count" => @count }
  end

  # a String-valued Hash, its ivar written through an attr_writer
  def labels
    { "label" => @label }
  end

  # an Integer-keyed Hash, answered by an explicit return
  def by_code
    return { 1 => @code }
  end

  # methods whose value is a call of one of them
  def summary
    to_h
  end

  def names
    labels
  end

  # the Hash through a local
  def copy
    h = { "count" => @count, "more" => 2 }
    h
  end

  # an ivar a method's own write widens, without a writer
  def bump
    n = Source.with { |c| c.code }
    @code = @code + n
  end
end

# The writes that widen the ivars. They never run: Source is defined nowhere,
# so its block calls answer values of no type.
def refresh(t)
  n = Source.with { |c| c.changes }
  t.count = t.count + n
  t.label = n
  t.bump
  t.to_h
end

t = Tally.new
p t.to_h, t.labels, t.by_code, t.summary, t.copy

t.count = t.count + 5
t.label = "five"
p t.to_h, t.labels, t.summary, t.names, t.copy

# each Hash in a caller's local, and in a caller's return
h = t.to_h
h["extra"] = 2
p h, h["count"] + 1, t.to_h.size

l = t.labels
l["other"] = "six"
p l, l.size

c = t.by_code
c[2] = 8
p c, c.keys

def snapshot(t)
  s = t.to_h
  s["at"] = 3
  s
end
p snapshot(t)

p(ARGV.size > 5 ? refresh(t) : 0)
