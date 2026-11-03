# A kept block holds its call's later run however many blocks that run at
# once lie between it and the call (64 here, as deep as the inlining goes).
class T; end
class Keep
  def each(&b) = (@b = b; self)
  def run = @b.call
end
class Now
  def each = yield
end
x = Keep.new
y = Now.new
x.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { y.each { p T.z } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } }
def T.z = :z
x.run
