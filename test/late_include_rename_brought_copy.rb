# A later include goes in front of an earlier include's copy of a method,
# where that earlier module brought another module's method of the name
# as a hidden copy behind its own: the earlier copy takes a hidden name,
# and the brought module's hidden copy keeps its own, which the earlier
# copy's super calls (two copies took one name and the C did not compile).
module B; def a = [:b, super]; end
module V; include B; def a = [:v, super]; end
module X; def a = [:x, super]; end
class P; def a = :p; end
class K < P; end
if ARGV.empty?
  class K; include V; end
end
if ARGV.empty?
  class K; prepend B; end
end
class K; include X; end
p K.new.a
