# A value-position `begin` whose tail is a writer call (or nil) is
# void/nil-typed: the begin result temp -- and the ivar slot it flows into --
# must widen to poly instead of C `void` (error: incomplete type 'void').
class Boot
  def name=(v)
    @n = v
    nil
  end

  def n
    @n
  end

  def init_a(v)
    @a = begin
      self.name = v
      nil
    end
  end

  def a
    @a
  end

  def init_b(v)
    @b = begin
      self.name = v
    end
  end
end

b = Boot.new
b.init_a(7)
p b.a
p b.n

b2 = Boot.new
b2.init_b(9)
p b2.n
