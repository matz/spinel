def convert(value)
  p String(value)
rescue ArgumentError, TypeError => error
  p [error.class, error.message]
end

convert(:abc)

class Symbol
  def to_s(required)
    +"unused"
  end
end

convert(:abc)

class Symbol
  def to_s(optional = +"optional")
    optional
  end
  private :to_s
end

convert(:abc)

class Symbol
  undef_method :to_s
end

convert(:abc)
begin
  p String(:abc)
rescue TypeError => error
  p [error.class, error.message]
end

class Symbol
  def to_str
    +"recovered"
  end
  private :to_str
end

convert(:abc)
