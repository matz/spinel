value = [1, "xxhixx"][1]
p value.strip("x")

class String
  def strip(*sets) = :override
end
p value.strip("x")

class String
  undef strip
end
begin
  p value.strip("x")
rescue NoMethodError => error
  p [error.name, error.args, error.receiver.equal?(value)]
end

class String
  def strip(*sets) = :restored
end
p value.strip("x")
