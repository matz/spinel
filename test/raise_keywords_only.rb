c = RuntimeError.new("c")
k = :cause
h = {cause: c}
begin; raise(k => c); rescue Exception => e; p [e.class, e.message, e.cause]; end
begin; raise(**h); rescue Exception => e; p [e.class, e.message, e.cause]; end
begin; raise(cause: c); rescue Exception => e; p [e.class, e.message, e.cause]; end
begin; raise; rescue Exception => e; p [e.class, e.message]; end
begin
  begin; raise "x"; rescue; raise(cause: c); end
rescue Exception => e; p [e.class, e.message, e.cause]; end
begin
  begin; raise "x"; rescue; raise(**h); end
rescue Exception => e; p [e.class, e.message, e.cause]; end
e0 = {}
o = {x: 1}
begin; raise(**e0); rescue Exception => e; p [e.class, e.message]; end
begin; raise(**o); rescue Exception => e; p [e.class, e.message]; end
begin; raise(x: 1); rescue Exception => e; p [e.class, e.message]; end
begin; raise(cause: nil); rescue Exception => e; p [e.class, e.message, e.cause]; end
begin; fail(cause: RuntimeError.new("f")); rescue Exception => e; p [e.class, e.message, e.cause]; end
begin; raise(x: 1, cause: nil); rescue Exception => e; p [e.class, e.message]; end
def only_cause(c)
  raise cause: c
rescue ArgumentError => e
  [e.class, e.message]
end
p only_cause(RuntimeError.new("m"))
def from_hash(h)
  raise(**h)
rescue => e
  [e.class, e.message]
end
p from_hash({})
p from_hash({cause: nil})
p from_hash({y: 2})
3.times do |i|
  begin
    raise(cause: (i.even? ? nil : RuntimeError.new("loop")))
  rescue ArgumentError => e
    p [i, e.message, e.cause&.message]
  end
end
