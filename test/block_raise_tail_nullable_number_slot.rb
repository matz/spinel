# A block that only raises, spliced where the yield's value is a nullable
# Integer or Float (another site's block answers a number, and the value is
# read as it is, by `p`): the splice's filler after the raise is the
# number's nil, not a bare 0, which did not fit it and the C did not build.
def y_int
  yield 1
end

def y_float
  yield 1.5
end

def b_int(&blk)
  blk.call(2)
end

def b_float(&blk)
  blk.call(2.5)
end

def tries
  n = 0
  begin
    n += 1
    yield n
  rescue RuntimeError
    retry if n < 3
    :gave_up
  end
end

p y_int { |t| t }
p((y_int { |t| raise "int" } rescue $!.message))
p y_float { |t| t }
p((y_float { |t| raise "float" } rescue $!.message))
p b_int { |t| t * 10 }
p((b_int { |t| raise "blk int" } rescue $!.message))
p b_float { |t| t * 2 }
p((b_float { |t| raise "blk float" } rescue $!.message))
p tries { |t| raise "again" if t < 2; t }
p tries { |t| raise "always" }
p((y_int { |t| if t > 0 then raise "nested" else raise "other" end } rescue $!.message))
