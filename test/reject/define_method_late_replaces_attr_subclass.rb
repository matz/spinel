# A define_method in a method's body that replaces an attr_reader is read
# through a subclass too: before `pin!` runs, CRuby's S.new.l calls the
# reader, after it the block (it prints 1, then 5 twice). A subclass's call
# keeps the reader's version, so the call is refused, not answered wrongly.
class R
  attr_reader :l
  def initialize = @l = 1
  def self.pin! = define_method(:l) { 5 }
end
class S < R; end
p S.new.l
R.pin!
p S.new.l
p R.new.l
