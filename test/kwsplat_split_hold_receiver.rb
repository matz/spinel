# The ** operand's to_hash runs after the receiver and the positional operands, and a Hash that is no literal may call it, so a variable read ahead of it is held; a literal hash that holds a **self too.
class C
  def initialize
    @a = [1]
    @b = :old
  end
  def to_hash
    @a = [99]
    @b = :new
    {}
  end
  def go
    r = @a.push(@b, **self)
    p r, @a
  end
  def go2
    r = @a.push(2, **self)
    p r
  end
end
C.new.go
C.new.go2
class C2
  def initialize = @a = [1]
  def to_hash
    @a = [99]
    {}
  end
  def go
    r = @a.push(2, **{**self})
    p r
  end
end
C2.new.go
