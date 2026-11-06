# An alias of an accessor answers, as CRuby does: #owner the class or module
# whose body holds the alias, and #source_location the attr_* call's line
# for a class's accessor, nil for one a module declares (CRuby wraps an
# alias of a module's method in an entry of its own, and reads an attr's
# position off the attr's own entry only).
module M
  attr_reader :x
  alias y x
end
class C
  include M
  alias z x
end
module P; attr_reader :q; end
class D
  prepend P
  alias r q
end
module Q; attr_reader :v; end
module R
  include Q
  alias w v
end
class E; include R; end
class F; attr_reader :a; end
class G < F; end
class G; alias b a; end

def show(s) = p(s ? [File.basename(s[0]), s[1]] : s)

o = C.new
p o.method(:x).owner, o.method(:y).owner, o.method(:z).owner
show o.method(:x).source_location
show o.method(:y).source_location
show o.method(:z).source_location
p D.new.method(:r).owner
show D.new.method(:r).source_location
p E.new.method(:w).owner
show E.new.method(:w).source_location
p G.new.method(:b).owner
show G.new.method(:b).source_location
