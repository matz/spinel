# spinel: reject-unsupported: a bare `super` into Module#include? from a `(...)` method whose calls pass keywords is not supported
# A `(...)` method's slots hold each keyword's value, where Module#include?
# is handed the one Hash its keywords make (CRuby's TypeError, wrong
# argument type Hash): refused rather than answered from the value.
module M
  def self.include?(...) = super
end
begin
  p M.include?(k: Kernel)
rescue TypeError => e
  p e.message
end
