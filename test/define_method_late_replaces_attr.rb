# A define_method in a method's body can replace an attr_reader: the reader
# answers until the method runs, the block after it (CRuby prints 1, 5).
class R
  attr_reader :level
  def initialize = @level = 1
  def self.pin! = define_method(:level) { 5 }
end
p R.new.level
R.pin!
p R.new.level
