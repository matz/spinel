# define_method with a computed name and a literal block makes a new
# method of the block, whose parameters and value already meet what the
# share walk does not see; no method the program defines becomes reachable
# by that name, so the walk joins nothing else (#9627).
class Router
  %w[index show].each do |action|
    define_method("#{action}_path") { |id| "/#{action}/#{id}" }
  end
end
r = Router.new
puts r.index_path(1), r.show_path(2)

def shout(s) = s.upcase!
def lookup(env = ENV) = env["HOME"]
t = +"hi"
shout(t)
p t, lookup.nil?
