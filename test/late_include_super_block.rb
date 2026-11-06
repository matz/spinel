# A later include whose module method passes a block to its super: the
# hidden method behind the copy, which runs until the include has run,
# takes the call's block and hands it on, as the copy's super reaches the
# superclass's method that yields.
module V; def a = [:v, super { |x| x * 2 }]; end
class P; def a = yield(3); end
class K < P; end
p K.new.a { |x| x }
class K; include V; end
p K.new.a { |x| x }

# the module's method yields itself
module W; def b = yield(10); end
class Q; def b = yield(4); end
class L < Q; end
p L.new.b { |x| x + 1 }
class L; include W; end
p L.new.b { |x| x + 1 }

# the module defines the method again, and its later version's super
# hands the call's block on
module X; def c = [:x, super { |v| v * 2 }]; end
class R; def c = yield(3); end
class M < R; end
p M.new.c { |v| v }
class M; include X; end
p M.new.c { |v| v }
module X; def c = [:x2, super]; end
p M.new.c { |v| v + 100 }

# a later prepend's module method whose super reaches the superclass's
# method through the class's own
module Y; def d = [:y, super]; end
class S; def d = yield(3); end
class N < S; def d = super; end
p N.new.d { |v| v }
class N; prepend Y; end
p N.new.d { |v| v + 100 }
