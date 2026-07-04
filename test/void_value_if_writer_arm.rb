# A value-position `if`/ternary whose arm is a writer call (doom's
# `self.fullscreen = value if respond_to?(...)` used as an expression) is
# void-typed: the result temp must widen to poly instead of being declared
# `void _tN` (error: incomplete type 'void'). The untaken branch reads nil.
class Win
  def fullscreen=(v)
    @fs = v
    nil
  end

  def fs
    @fs
  end

  def apply(v, cond)
    @last = (self.fullscreen = v if cond)
  end

  def last
    @last
  end

  def tern(v, cond)
    y = (cond ? (self.fullscreen = v) : nil)
    y
  end

  def local_if(v, cond)
    y = (self.fullscreen = v if cond)
    y
  end
end

w = Win.new
w.apply(true, true)
p w.fs

w2 = Win.new
w2.apply(true, false)
p w2.last
p w2.fs

w3 = Win.new
p w3.tern(5, false)
p w3.fs
w3.tern(5, true)
p w3.fs

w4 = Win.new
p w4.local_if(9, false)
w4.local_if(9, true)
p w4.fs
