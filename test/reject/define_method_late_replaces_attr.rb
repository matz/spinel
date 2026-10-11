# The method replaced can have no def: an attr_reader (CRuby prints 1, 5).
class R
  attr_reader :level
  def initialize = @level = 1
  def self.pin! = define_method(:level) { 5 }
end
p R.new.level
R.pin!
p R.new.level
