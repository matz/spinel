# self.class.at(...) in an instance method of a Time subclass nothing inherits
# (tzinfo's TimeWithOffset#round): self.class is that class, so the maker
# makes an instance of it, as Stamp.at does; it raised NoMethodError for at.
class Stamp < Time
  def again
    self.class.at(to_i, subsec * 1_000_000)
  end

  def now_again
    self.class.now.class
  end
end

class Mark < Time
  def again
    self.class.at(to_i)
  end
end

class Mark2 < Mark
end

s = Stamp.at(86400, 500000)
t = s.again
p t.class
p t.to_i
p t.usec
p s.now_again
