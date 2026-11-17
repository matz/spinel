# spinel: reject-unsupported: super in `self.new` of a Struct, Data, exception or native class
# The same with a def body and a keyword rest, in a reopened KeyError, and a use that
# only reads the class.
class KeyError
  def self.new(m = nil, **k)
    super
  end
end
p KeyError.new("x").class
