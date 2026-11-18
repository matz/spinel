module Layer
  protected
  def size = 18
end
class Friend
  prepend Layer
  def check(v) = v.size
  def pub(v) = v.public_send(:size)
end
class Stranger
  def check(v) = v.size
end
def sample(f,x,v)
  begin
    p [:friend,f.check(v)]
  rescue NoMethodError
    p :friend_closed
  end
  begin
    p [:public,f.pub(v)]
  rescue NoMethodError
    p :public_closed
  end
  begin
    p [:stranger,x.check(v)]
  rescue NoMethodError
    p :stranger_closed
  end
end
v=+"abc"
f=Friend.new
x=Stranger.new
sample(f,x,v)
class String
  prepend Layer if (ENV.fetch("SPINEL_TEST_EARLY_PREPEND", "yes") == "yes")
end
sample(f,x,v)
