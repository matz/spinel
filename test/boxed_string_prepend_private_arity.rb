module Outside
  def length = 90
end
class Peer
  def length = 40
end
def sample(v)
  begin
    p [:regular,v.length(7,8)]
  rescue NoMethodError => e
    p [:hidden,e.name,e.args]
  rescue ArgumentError
    p :arity
  end
  begin
    p [:blind,v.send(:length,7,8)]
  rescue ArgumentError
    p :arity
  end
end
v=[+"abc",Peer.new][0]
class String
  prepend Outside if ENV['LAYER']
end
sample(v)
class String
  def length = 22
  private :length if (ENV.fetch("SPINEL_TEST_EARLY_PREPEND", "yes") == "yes")
end
sample(v)
