# `rescue m::Err` with m a value, where classes in two scopes are named
# Err: which one it names is the one m holds, at run time, and a rescue
# list takes no expression of that kind, so the program is refused
# instead of rescuing the leaf-named class (#10955).
class Err < StandardError; end
module Ext; class Err < StandardError; end; end
m = Ext
begin
  raise Ext::Err
rescue m::Err
  puts "ext"
end
