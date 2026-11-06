# Compiled with --defer-refusals: a send with a runtime method name in a
# method is deferred like any other refusal -- the method raises
# NotImplementedError naming it, and a program that never calls it
# (actionpack's polymorphic route helpers, `target.public_send
# get_method_for_string(str)`) builds and runs. Without the flag it is
# refused.
# spinel: defer-refusals: 2:start true after
module Helpers
  def helper_for(kind)
    public_send(["path", "for", kind.to_s].join("_"))
  end
end
class Routes
  extend Helpers
  def self.path_for_post = "/posts"
end

puts "start"
begin
  Routes.helper_for(:post)
rescue NotImplementedError => e
  puts e.message.include?("runtime method name")
end
puts "after"
proc { "a" }.ruby2_keywords
puts "not reached"
