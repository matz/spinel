# A bare `super` passes the method's keyword parameters as keywords, which a
# builtin method whose own parameters list no keyword (Hash#merge) cannot be
# given in the call it is spelled as: refused, with the explicit form suggested.
# spinel: reject-subclass: a bare `super` from a method with keyword or `**` parameters into a builtin whose keyword
class Registry < Hash
  def merge(*others, strict: false)
    super
  end
end

p Registry.new.merge({ a: 1 }, strict: true)
