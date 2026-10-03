# `raise e, cause: other` replaces the cause e already has, also when a
# rescue clause catches it; `cause: nil` leaves it. A frozen e is raised as
# a copy carrying the new cause, and a cause whose chain leads back to e
# does not replace e's.
def with_cause(c)
  e = RuntimeError.new("e")
  begin
    raise e, cause: c
  rescue
  end
  e
end

seed = RuntimeError.new("seed")
other = RuntimeError.new("other")

e = with_cause(seed)
p e.cause.equal?(seed)
begin
  raise e, cause: other
rescue => r
  p [r.equal?(e), r.cause.equal?(other)]
end
p e.cause.equal?(other)

e = with_cause(seed)
begin
  raise e, cause: nil
rescue
end
p e.cause.equal?(seed)

e = with_cause(seed)
e.freeze
begin
  raise e, cause: other
rescue => r
  p [r.equal?(e), r.cause.equal?(other), e.cause.equal?(seed)]
end

e = with_cause(seed)
child = with_cause(e)
begin
  raise e, cause: child
rescue
end
p e.cause.equal?(seed)
