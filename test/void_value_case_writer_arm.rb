# A value-position `case` whose arms are writer calls (or nil) is
# void/nil-typed: the case result temp must widen to poly instead of being
# declared `void _crN` (error: incomplete type 'void'). Unmatched reads nil.
class Conf
  def name=(v)
    @n = v
    nil
  end

  def n
    @n
  end

  def set(k, v)
    @x = case k
         when 1 then self.name = v
         when 2 then self.name = v * 2
         end
  end

  def x
    @x
  end

  def nil_arm(k)
    case k
    when 1 then nil
    end
  end
end

c = Conf.new
c.set(3, 5)
p c.x
p c.n
c.set(1, 5)
p c.n
c.set(2, 5)
p c.n
p c.nil_arm(1)
p c.nil_arm(2)
