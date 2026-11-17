# spinel: reject-unsupported: super in `self.new` of a Struct, Data, exception or native class
# A `def self.new` that calls super in a reopened builtin exception (K-056): the reopening
# has no struct of its own, so it was a C error (unknown type name sp_NoMethodError) and is now
# refused where the super is written, as it is for a program subclass of an exception.
class NoMethodError
  def self.new(*a, **k) = super
end
p NoMethodError.new("x").message
