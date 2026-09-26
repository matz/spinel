# A user method called on an object-typed parameter that a caller passes nil
# raises NoMethodError for nil, as in CRuby. The typed emission ran the
# method with a NULL self: one that read no ivar answered, and one that did
# read through NULL (#5088). The names nil answers itself still answer.

class U
  attr_reader :n
  def initialize = @n = 7
  def hats = [1, 2]
  def weight = @n * 2
end

def count(u) = u.hats.size
def heavy(u) = u.weight
def reader(u) = u.n
def relay(u) = count(u)
def safe(u) = [u.nil?, u.to_s, u&.hats]

p count(U.new)
p((count(nil) rescue "NoMethodError"))
p heavy(U.new)
p((heavy(nil) rescue "NoMethodError"))
p reader(U.new)
p((reader(nil) rescue "NoMethodError"))
p((relay(nil) rescue "NoMethodError"))
p safe(nil)
p safe(U.new).first
begin
  count(nil)
rescue NoMethodError => e
  p e.message
end
