# A bare super in an exception's initialize with a rest parameter hands on
# the rest's elements, as super(*args) does.
class AppError < StandardError
  def initialize(*args)
    super
  end
end
p AppError.new("x").message
p AppError.new.message
begin
  raise AppError, "raised"
rescue AppError => e
  p e.message, e.class
end

class KwError < StandardError
  def initialize(*a, **k)
    super
    @extra = k[:extra]
  end
  attr_reader :extra
end
kw = KwError.new("kw")
p kw.message, kw.extra

class Lead < StandardError
  def initialize(code, *rest)
    @code = code
    super
  end
  attr_reader :code
end
l = Lead.new(7)
p l.code, l.message

class RuntimeError
  def initialize(*a)
    super
  end
end
begin
  raise "boom"
rescue => e
  p e.message
end
p RuntimeError.new("made").message

class Wide < ArgumentError
  def initialize(*args)
    super
    @n = args.size
  end
  attr_reader :n
end
w = Wide.new("w")
p w.message, w.n
