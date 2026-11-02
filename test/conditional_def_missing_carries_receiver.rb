# A method a run-time condition may leave undefined raises NoMethodError
# that names the method and carries the object the call was made on, as
# CRuby's does: an instance's, a subclass instance's and a class's.
class C
  if ARGV.size > 5
    def late = 1
  end
end
class D < C; end
o = C.new
begin; o.late; rescue NoMethodError => e; p e.name; p e.receiver.equal?(o); p e.message; end
d = D.new
begin; d.late; rescue NoMethodError => e; p e.name; p e.receiver.class; p e.receiver.equal?(d); end
class << C
  if ARGV.size > 5
    def clate = 1
  end
end
begin; D.clate; rescue NoMethodError => e; p e.name; p e.receiver; p e.message; end
# ...and the call's arguments (#args), also through NoMethodError.new's own
# positional arguments
class E
  if ARGV.size > 5
    def late(x, y) = x
    def rest(*a, **k) = a
  end
end
e1 = E.new
begin; e1.late(1, "x"); rescue NoMethodError => e; p e.args; p e.receiver.equal?(e1); end
begin; e1.rest(2, 3); rescue NoMethodError => e; p e.args; end
begin; e1.rest(2, k: 1); rescue NoMethodError => e; p e.args; end
begin; e1.rest(2); rescue NoMethodError => e; p e.args; end
begin; e1.rest(2, **{}); rescue NoMethodError => e; p e.args; end
err = NoMethodError.new("m", :late, [1, :a], receiver: e1)
p [err.name, err.args, err.receiver.equal?(e1)]
p NoMethodError.new("m", :late, ["x", 2]).args
# ...an argument whose box allocates (a Range) is held while the error is built
class F
  if ARGV.size > 5
    def span(r, s) = r
  end
end
f = F.new
3.times { |i| begin; f.span(i..i + 2, "s#{i}"); rescue NoMethodError => e; p [e.args, e.receiver.equal?(f)]; end }
# ...and NoMethodError.new's own empty argument list, built beside the receiver
2.times { |i| begin; raise NoMethodError.new("m#{i}", :late, [], receiver: f); rescue NoMethodError => e; p [e.message, e.name, e.args, e.receiver.equal?(f)]; end }
