# An error raised inside a rescue body carries the exception being handled
# as its cause also when nothing rescues it and no at_exit hook is armed: a
# reopened #message that reads it in the report gets it, as in CRuby, not nil.
class KeyError
  def message
    "key=#{key.inspect} cause=#{cause.inspect}"
  end
end

puts "before"
begin
  raise ArgumentError, "inner"
rescue ArgumentError
  {}.fetch(:a)
end
