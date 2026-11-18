module A
  protected
  def size = 11
end
module B
  protected
  def size = 22
end
class FriendOfA
  include A
  def read(v) = v.size
end
class FriendOfB
  include B
  def read(v) = v.size
end
class String
  prepend A if (ENV["SPINEL_TEST_EARLY_PREPEND"] == "yes")
  prepend B unless (ENV["SPINEL_TEST_EARLY_PREPEND"] == "yes")
end
def sample(friend,v)
  p friend.read(v)
rescue NoMethodError
  p :closed
end
s = +"abc"
sample(FriendOfA.new,s)
sample(FriendOfB.new,s)
